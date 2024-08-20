#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "string.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/adc.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "esp_http_client.h"

#define UART_PORT_NUM      UART_NUM_0  
#define UART_BAUD_RATE     115200
#define UART_BUFFER_SIZE   1024

#define LED_GPIO_PIN       2
#define SENSOR_PIN ADC1_CHANNEL_6

#define EXAMPLE_ESP_WIFI_SSID      "Redmi"
#define EXAMPLE_ESP_WIFI_PASS      "mikdat123" 
#define EXAMPLE_ESP_MAXIMUM_RETRY  5

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define URL "http://192.168.185.125/moisture/moisture_data.php"  

static const char *TAG = "SENSOR_DATA";

static int s_retry_num = 0;

static QueueHandle_t uart_queue;
int moisture,sensorAnalog;
const int device_no = 1;

void configure_adc() {
    adc1_config_width(ADC_WIDTH_BIT_12);  // ADC veri genişliğini 12 bit olarak ayarla
    adc1_config_channel_atten(SENSOR_PIN, ADC_ATTEN_DB_11);  // Kanal zayıflatmasını ayarla
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;

        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();

    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGI(TAG, "UNEXPECTED EVENT");
    }
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT, header sent");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Copy out the data
                if (evt->data) {
                    ESP_LOGI(TAG, "Data: %.*s", evt->data_len, (char*)evt->data);
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT: // Added to handle redirect events
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

int read_sensor_value() {
    int value = adc1_get_raw(SENSOR_PIN);
    int min_value = 1400;
    int max_value = 3022; 

    int moisture = 100-(100 * (max_value - value) / (max_value - min_value));

    if (moisture < 0) {
        moisture = 0;
    }

    return moisture;
}

void post_data(int device_no, int moisture) {
    esp_http_client_config_t config = {
        .url = URL,
        .method = HTTP_METHOD_POST,
        .event_handler = _http_event_handler,
        .cert_pem = NULL, // Sertifika doğrulamasını geçersiz kılmak için NULL
        .skip_cert_common_name_check = true, // Sertifika ismi kontrolünü atla
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char post_data[100];
    int post_data_length = snprintf(post_data, sizeof(post_data), "device_no=%d&moisture=%d", device_no, moisture);
    
    if (post_data_length >= sizeof(post_data)) {
        ESP_LOGE(TAG, "POST data size exceeds buffer limit");
        esp_http_client_cleanup(client);
        return;
    }

    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");


    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Success. Status = %d, content_length = %lld",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}


void uart_event_task(void *pvParameters) {
    uart_event_t event;
    uint8_t data[UART_BUFFER_SIZE];
    int len;

    for (;;) {
        if (xQueueReceive(uart_queue, (void *) &event, portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA:
                    len = uart_read_bytes(UART_PORT_NUM, data, UART_BUFFER_SIZE, 100 / portTICK_PERIOD_MS);
                    if (len > 0) {
                        data[len] = '\0'; 
                        printf("Received: %s\n", data);

                        if (strncmp((char *) data, "Led_on", strlen("Led_on")) == 0) {
                            gpio_set_level(LED_GPIO_PIN, 1);
                            vTaskDelay(10 / portTICK_PERIOD_MS);
                            printf("LED yandi\n");

                        } else if (strncmp((char *) data, "Led_off", strlen("Led_off")) == 0) {
                            gpio_set_level(LED_GPIO_PIN, 0);
                            vTaskDelay(10 / portTICK_PERIOD_MS);
                            printf("LED sondu\n");

                        } else if (strncmp((char *) data, "ms_sensor", strlen("ms_sensor")) == 0){
                                sensorAnalog = read_sensor_value();
                                printf("Sensor degeri: %d", sensorAnalog);
                                int num = adc1_get_raw(SENSOR_PIN);
                                printf("deger: %d\n", num);
                                vTaskDelay(10 / portTICK_PERIOD_MS);
                                post_data(device_no,sensorAnalog);
                                vTaskDelay(10 / portTICK_PERIOD_MS);
                                if(sensorAnalog<50){
                                    gpio_set_level(LED_GPIO_PIN, 1);
                                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                                    printf("Nem cok az. Sulama gerekli.\n");
                                    gpio_set_level(LED_GPIO_PIN, 0);
                                }
                        }
                    }
                    break;

                default:
                    break;
            }
        }
    }
}

void app_main(void) {

    // UART konfigürasyonu
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    // UART yapılandırması
    uart_param_config(UART_PORT_NUM, &uart_config);
    uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PORT_NUM, (unsigned int) UART_BUFFER_SIZE * 2, (unsigned int) UART_BUFFER_SIZE * 2, 10, &uart_queue, 0);

    // NVS flash initialization
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    // LED GPIO konfigürasyonu
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LED_GPIO_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // UART olay işleme görevini başlat
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 10, NULL);
}
