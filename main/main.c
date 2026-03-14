#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "inttypes.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include <esp_http_server.h>

// --- 1. Configuration ---
#define WIFI_SSID           "AAKASH"
#define WIFI_PASS           "9322508241"
#define MAX_RETRY           5
#define DHT_DATA_PIN        4
#define OBSTACLE_SENSOR_PIN 27


static EventGroupHandle_t s_wifi_event_group;
static QueueHandle_t system_data_queue;
static SemaphoreHandle_t obstacle_semaphore = NULL;
static const char* HTML_TEMPLATE = 
    "<html>"
    "<head>"
    "<meta http-equiv='refresh' content='2'>" // Auto-refresh every 2 seconds
    "<style>body{font-family:sans-serif; text-align:center; padding-top:50px; background:#f4f4f4;}"
    ".card{background:white; padding:20px; border-radius:10px; display:inline-block; shadow:0 4px 8px 0 rgba(0,0,0,0.2);}</style>"
    "<title>ESP32 IoT Gateway</title>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>Live IoT Dashboard</h1>"
	"<h2>Temperature: <span style='color:red;'>%d&deg;C</span></h2>" 
    "<h2>Humidity: <span style='color:blue;'>%d%%</span></h2>"
    "<h2>Status: <span style='color:orange;'>%s</span></h2>"
    "<p>System Uptime: %" PRIu32 " seconds</p>"
    "</div>"
    "</body>"
    "</html>";


#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "WIFI_GATEWAY";
static int s_retry_num = 0;

// --- 3. Data Structure ---
typedef struct {
    int temperature;
    int humidity;
    bool obstacle_detected;
    uint32_t uptime_seconds;
} system_data_t;




// Wi-Fi Event Handler
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to AP...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t get_handler(httpd_req_t *req) {
    system_data_t latest_data;
    char response[1024]; // Buffer to hold the final HTML

    // 1. PEEK: Look at the mailbox (Queue) for the latest sensor packet
    // We wait up to 1 second (1000ms) to ensure we don't send 0s
    if (xQueuePeek(system_data_queue, &latest_data, pdMS_TO_TICKS(1000)) == pdTRUE) {
        
        // 2. FILL: This is where we officially USE the HTML_TEMPLATE
        // We replace %d and %s in the template with real values
        snprintf(response, sizeof(response), HTML_TEMPLATE, 
                 latest_data.temperature, 
                 latest_data.humidity, 
                 latest_data.obstacle_detected ? "!!! OBSTACLE !!!" : "Clear",
                 latest_data.uptime_seconds);
    } else {
        // Fallback if the sensor task hasn't sent anything yet
        snprintf(response, sizeof(response), "<h1>System Initializing... Refresh in 2s</h1>");
    }

    // 3. SEND: Push the HTML to your phone's browser
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


void start_webserver() {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        httpd_uri_t uri_get = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get);
        ESP_LOGI(TAG, "Web Server Started Successfully!");
    }
}



// IR Sensor ISR
static void IRAM_ATTR ir_sensor_isr_handler(void* arg) {
    xSemaphoreGiveFromISR(obstacle_semaphore, NULL);
}

// Custom DHT11 Timing Logic
static int wait_for_signal(int pin, int level, int timeout_us) {
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if (esp_timer_get_time() - start > timeout_us) return -1; 
    }
    return (int)(esp_timer_get_time() - start);
}

int fetch_dht11_data(int *temp_out, int *hum_out) {
    uint8_t bits[5] = {0};
    portMUX_TYPE myMutex = portMUX_INITIALIZER_UNLOCKED;

    taskENTER_CRITICAL(&myMutex);
    
    gpio_set_direction(DHT_DATA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_DATA_PIN, 0); 
    esp_rom_delay_us(20000); 
    gpio_set_level(DHT_DATA_PIN, 1); 
    esp_rom_delay_us(30); 
    
    gpio_set_direction(DHT_DATA_PIN, GPIO_MODE_INPUT);
    if (wait_for_signal(DHT_DATA_PIN, 0, 100) == -1) { taskEXIT_CRITICAL(&myMutex); return -1; }
    if (wait_for_signal(DHT_DATA_PIN, 1, 100) == -1) { taskEXIT_CRITICAL(&myMutex); return -1; }
    if (wait_for_signal(DHT_DATA_PIN, 0, 100) == -1) { taskEXIT_CRITICAL(&myMutex); return -1; }

    for (int i = 0; i < 40; i++) {
        if (wait_for_signal(DHT_DATA_PIN, 1, 100) == -1) { taskEXIT_CRITICAL(&myMutex); return -1; }
        int high_duration = wait_for_signal(DHT_DATA_PIN, 0, 100);
        if (high_duration == -1) { taskEXIT_CRITICAL(&myMutex); return -1; }
        if (high_duration > 40) bits[i / 8] |= (1 << (7 - (i % 8)));
    }

    taskEXIT_CRITICAL(&myMutex);

    if (bits[4] == ((bits[0] + bits[1] + bits[2] + bits[3]) & 0xFF)) {
        *hum_out = bits[0];
        *temp_out = bits[2];
        return 0; 
    }
    return -2; 
}

// --- 5. Tasks ---

void sensor_task(void *pvParameters) {
    system_data_t sensor_packet;
    int temp, hum;
    
    while (1) {
        // 1. Get DHT11 Data
        if (fetch_dht11_data(&temp, &hum) == 0) {
            sensor_packet.temperature = temp;
            sensor_packet.humidity = hum;
        }

        // 2. CHECK GPIO PIN DIRECTLY
        // Since your IR sensor is Active Low (0 = Obstacle), we check for 0.
        if (gpio_get_level(OBSTACLE_SENSOR_PIN) == 0) {
            sensor_packet.obstacle_detected = true;
            ESP_LOGW("SENSOR", "Obstacle detected for Dashboard!");
        } else {
            sensor_packet.obstacle_detected = false;
        }

        sensor_packet.uptime_seconds = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        
        // 3. Update the Queue
        // Use xQueueOverwrite to ensure the web server always sees the newest data.
        xQueueOverwrite(system_data_queue, &sensor_packet); 

        vTaskDelay(pdMS_TO_TICKS(1000)); // Scan every 1 second for faster dashboard updates
    }
}

void wireless_telemetry_task(void *pvParameters) {
    system_data_t received_data;
    
    // Wait until Wi-Fi is officially connected AND has an IP address
    ESP_LOGI(TAG, "Waiting for stable IP address...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    // Start the webserver ONLY after the bit is set
    start_webserver(); 

    while(1) {
        if (xSemaphoreTake(obstacle_semaphore, 0) == pdTRUE) {
            ESP_LOGW("ALERT", "!!! OBSTACLE DETECTED !!!");
        }

        if (xQueueReceive(system_data_queue, &received_data, pdMS_TO_TICKS(100))) {
            printf("LOG -> Uptime: %" PRIu32 "s | T: %dC | H: %d%%\n", 
                    received_data.uptime_seconds, received_data.temperature, received_data.humidity);
        }
    }
}


// --- 6. Initialization ---

void init_ir_sensor(int pin) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    obstacle_semaphore = xSemaphoreCreateBinary();
    gpio_install_isr_service(0);
    gpio_isr_handler_add(pin, ir_sensor_isr_handler, (void*) pin);
}

void app_main(void) {
    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. FreeRTOS Setup
    s_wifi_event_group = xEventGroupCreate();
   // Change queue size to 1 since we only care about the LATEST data for the dashboard
	system_data_queue = xQueueCreate(1, sizeof(system_data_t));
    init_ir_sensor(OBSTACLE_SENSOR_PIN);

    // 3. Network Setup
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    

    // 4. Launch Tasks
    xTaskCreatePinnedToCore(sensor_task, "sensor_task", 4096, NULL, 5, NULL, 1);
    xTaskCreate(wireless_telemetry_task, "wireless_task", 4096, NULL, 5, NULL);
}