#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "hal/ledc_periph.h"
#include "esp_rom_gpio.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#define LEDC_GPIO              CONFIG_LEDC_GPIO
#define LEDC_MODE              LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL           LEDC_CHANNEL_0
#define LEDC_TIMER             LEDC_TIMER_0
#define LEDC_DUTY_RES          ((ledc_timer_bit_t)CONFIG_LEDC_PWM_RES)
#define LEDC_FREQUENCY         CONFIG_LEDC_PWM_FREQ

static const char *TAG = "led_control";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static int s_brightness = 50;
static bool s_led_on = true;

// Helper to update hardware PWM based on current brightness and power state
void update_led_hardware(void) {
    uint32_t duty = 0;
    if (s_led_on) {
        uint32_t max_duty = (1 << CONFIG_LEDC_PWM_RES) - 1;
        duty = (s_brightness * max_duty) / 100;
    }
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

// Initialize LEDC (PWM)
void init_pwm(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_GPIO,
        .duty           = 0, // Initially off
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
    
    // Set drive strength to sharpen edges (does not affect GPIO Mux)
    gpio_set_drive_capability(LEDC_GPIO, CONFIG_LEDC_DRIVE_STRENGTH);
    ESP_LOGI(TAG, "LED GPIO drive strength set to %d", CONFIG_LEDC_DRIVE_STRENGTH);
    ESP_LOGI(TAG, "PWM Frequency: %d Hz, Resolution: %d bits", LEDC_FREQUENCY, CONFIG_LEDC_PWM_RES);

    // Apply Output Mode (Push-Pull vs Open-Drain) and Logic Inversion
#if CONFIG_LEDC_OUTPUT_MODE_OD
    gpio_set_direction(LEDC_GPIO, GPIO_MODE_OUTPUT_OD);
    ESP_LOGI(TAG, "LED GPIO configured in Open-Drain mode. External pull-up required.");
#else
    gpio_set_direction(LEDC_GPIO, GPIO_MODE_OUTPUT);
    ESP_LOGI(TAG, "LED GPIO configured in Push-Pull mode.");
#endif

    // Apply signal inversion if configured
    bool invert = false;
#if CONFIG_LEDC_INVERT_LOGIC
    invert = true;
    ESP_LOGI(TAG, "PWM Logic Inversion enabled.");
#endif

    // Re-link the LEDC signal to the GPIO (Target: ESP32-C6)
    // Signal indices are defined in hal/ledc_periph.h
    esp_rom_gpio_connect_out_signal(LEDC_GPIO, ledc_periph_signal[0].speed_mode[LEDC_MODE].sig_out_idx[LEDC_CHANNEL], invert, false);

    // Apply initial hardware state
    update_led_hardware();
}

// Set brightness helper (intensity: 0 - 100%)
void set_led_intensity(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    s_brightness = percent;
    update_led_hardware();
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// URI Handler: GET / (Serve Web Page)
esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

// REST Endpoint Handler: POST /api/brightness?value=0-100
esp_err_t set_brightness_handler(httpd_req_t *req) {
    char buf[32];
    int percent = 0;

    ESP_LOGD(TAG, "Received brightness request, URI: %s", req->uri);

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "value", param, sizeof(param)) == ESP_OK) {
            percent = atoi(param);
            ESP_LOGI(TAG, "Changing brightness to %d%%", percent);
            set_led_intensity(percent);

            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"success\"}");
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Invalid brightness request: missing or invalid 'value' parameter");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'value' parameter");
    return ESP_FAIL;
}

// REST Endpoint Handler: GET /api/brightness
esp_err_t get_brightness_handler(httpd_req_t *req) {
    char json_response[64];
    snprintf(json_response, sizeof(json_response), "{\"brightness\": %d}", s_brightness);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);
    return ESP_OK;
}

// REST Endpoint Handler: GET /api/led_info
esp_err_t get_led_info_handler(httpd_req_t *req) {
    char json_response[64];
    snprintf(json_response, sizeof(json_response), "{\"gpio\": %d}", LEDC_GPIO);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);
    return ESP_OK;
}

// REST Endpoint Handler: GET /api/power
esp_err_t get_power_handler(httpd_req_t *req) {
    char json_response[64];
    snprintf(json_response, sizeof(json_response), "{\"power\": \"%s\"}", s_led_on ? "on" : "off");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);
    return ESP_OK;
}

// REST Endpoint Handler: POST /api/power?value=on|off
esp_err_t set_power_handler(httpd_req_t *req) {
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "value", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "on") == 0) {
                s_led_on = true;
            } else if (strcmp(param, "off") == 0) {
                s_led_on = false;
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid 'value' parameter. Use 'on' or 'off'.");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "LED power set to %s", s_led_on ? "ON" : "OFF");
            update_led_hardware();
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"success\"}");
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'value' parameter");
    return ESP_FAIL;
}

// Start HTTP Server
httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting web server...");
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t brightness_uri = {
            .uri       = "/api/brightness",
            .method    = HTTP_POST,
            .handler   = set_brightness_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &brightness_uri);

        httpd_uri_t get_brightness_uri = {
            .uri       = "/api/brightness",
            .method    = HTTP_GET,
            .handler   = get_brightness_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &get_brightness_uri);

        httpd_uri_t get_led_info_uri = {
            .uri       = "/api/led_info",
            .method    = HTTP_GET,
            .handler   = get_led_info_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &get_led_info_uri);

        httpd_uri_t power_uri = {
            .uri       = "/api/power",
            .method    = HTTP_POST,
            .handler   = set_power_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &power_uri);

        httpd_uri_t get_power_uri = {
            .uri       = "/api/power",
            .method    = HTTP_GET,
            .handler   = get_power_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &get_power_uri);

        httpd_uri_t root_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root_uri);

        ESP_LOGI(TAG, "Web server started and URI registered");
    } else {
        ESP_LOGE(TAG, "Failed to start web server");
    }
    return server;
}

// Unified event handler for WiFi and IP events
static void event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Successfully got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Web UI available at: http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for changing brightness: POST http://" IPSTR "/api/brightness?value=50", 
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for getting brightness:  GET  http://" IPSTR "/api/brightness", 
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for getting LED port:    GET  http://" IPSTR "/api/led_info", 
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for changing power:      POST http://" IPSTR "/api/power?value=on", 
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for getting power:       GET  http://" IPSTR "/api/power", 
                 IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by setting threshold.authmode */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize PWM first so the LED works even without WiFi
    init_pwm();
    
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    // Start Webserver
    start_webserver();
    
    ESP_LOGI(TAG, "LED Driver application started.");
}