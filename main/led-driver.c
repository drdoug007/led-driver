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

#define NUM_CHANNELS           6
#define LEDC_MODE              LEDC_LOW_SPEED_MODE
#define LEDC_TIMER             LEDC_TIMER_0
#define LEDC_DUTY_RES          ((ledc_timer_bit_t)CONFIG_LEDC_PWM_RES)
#define LEDC_FREQUENCY         CONFIG_LEDC_PWM_FREQ

static const int LEDC_GPIOS[NUM_CHANNELS] = {
    CONFIG_LEDC_GPIO_0,
    CONFIG_LEDC_GPIO_1,
    CONFIG_LEDC_GPIO_2,
    CONFIG_LEDC_GPIO_3,
    CONFIG_LEDC_GPIO_4,
    CONFIG_LEDC_GPIO_5
};

static const char *TAG = "led_control";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static int s_brightness[NUM_CHANNELS] = {50, 50, 50, 50, 50, 50};
static bool s_led_on[NUM_CHANNELS] = {true, true, true, true, true, true};

// Helper to update hardware PWM based on current brightness and power state
void update_led_hardware(int ch) {
    if (ch < 0 || ch >= NUM_CHANNELS) return;
    
    uint32_t duty = 0;
    if (s_led_on[ch]) {
        uint32_t max_duty = (1 << CONFIG_LEDC_PWM_RES) - 1;
        duty = (s_brightness[ch] * max_duty) / 100;
    }
    ledc_set_duty(LEDC_MODE, (ledc_channel_t)ch, duty);
    ledc_update_duty(LEDC_MODE, (ledc_channel_t)ch);
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

    for (int i = 0; i < NUM_CHANNELS; i++) {
        ledc_channel_config_t ledc_channel = {
            .speed_mode     = LEDC_MODE,
            .channel        = (ledc_channel_t)i,
            .timer_sel      = LEDC_TIMER,
            .intr_type      = LEDC_INTR_DISABLE,
            .gpio_num       = LEDC_GPIOS[i],
            .duty           = 0, // Initially off
            .hpoint         = 0
        };
        ledc_channel_config(&ledc_channel);
        
        // Set drive strength
        gpio_set_drive_capability(LEDC_GPIOS[i], CONFIG_LEDC_DRIVE_STRENGTH);
        
        // Apply Output Mode
#if CONFIG_LEDC_OUTPUT_MODE_OD
        gpio_set_direction(LEDC_GPIOS[i], GPIO_MODE_OUTPUT_OD);
#else
        gpio_set_direction(LEDC_GPIOS[i], GPIO_MODE_OUTPUT);
#endif

        // Apply signal inversion if configured
        bool invert = false;
#if CONFIG_LEDC_INVERT_LOGIC
        invert = true;
#endif

        // Re-link the LEDC signal to the GPIO (Target: ESP32-C6)
        esp_rom_gpio_connect_out_signal(LEDC_GPIOS[i], ledc_periph_signal[0].speed_mode[LEDC_MODE].sig_out_idx[i], invert, false);

        // Apply initial hardware state
        update_led_hardware(i);
    }
    
    ESP_LOGI(TAG, "Initialized %d LED channels", NUM_CHANNELS);
    ESP_LOGI(TAG, "PWM Frequency: %d Hz, Resolution: %d bits", LEDC_FREQUENCY, CONFIG_LEDC_PWM_RES);
}

// Set brightness helper (intensity: 0 - 100%)
void set_led_intensity(int ch, int percent) {
    if (ch < 0 || ch >= NUM_CHANNELS) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    s_brightness[ch] = percent;
    update_led_hardware(ch);
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]   asm("_binary_favicon_ico_end");

// URI Handler: GET / (Serve Web Page)
esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

// URI Handler: GET /favicon.ico (Serve Icon)
esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_end - favicon_ico_start);
    return ESP_OK;
}

// REST Endpoint Handler: POST /api/brightness?value=0-100&ch=0-5
esp_err_t set_brightness_handler(httpd_req_t *req) {
    char buf[64];
    int percent = 0;
    int ch = 0;

    ESP_LOGD(TAG, "Received brightness request, URI: %s", req->uri);

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "ch", param, sizeof(param)) == ESP_OK) {
            ch = atoi(param);
        }
        if (httpd_query_key_value(buf, "value", param, sizeof(param)) == ESP_OK) {
            percent = atoi(param);
            if (ch >= 0 && ch < NUM_CHANNELS) {
                ESP_LOGI(TAG, "Changing brightness of channel %d to %d%%", ch, percent);
                set_led_intensity(ch, percent);

                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"status\":\"success\"}");
                return ESP_OK;
            }
        }
    }

    ESP_LOGW(TAG, "Invalid brightness request");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid parameters");
    return ESP_FAIL;
}

// REST Endpoint Handler: GET /api/brightness?ch=0-5
esp_err_t get_brightness_handler(httpd_req_t *req) {
    char buf[64];
    int ch = -1;
    char json_response[256];

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "ch", param, sizeof(param)) == ESP_OK) {
            ch = atoi(param);
        }
    }

    if (ch >= 0 && ch < NUM_CHANNELS) {
        snprintf(json_response, sizeof(json_response), "{\"channel\": %d, \"brightness\": %d}", ch, s_brightness[ch]);
    } else {
        // Return all channels
        int len = snprintf(json_response, sizeof(json_response), "{\"brightness\": [");
        for (int i = 0; i < NUM_CHANNELS; i++) {
            len += snprintf(json_response + len, sizeof(json_response) - len, "%d%s", 
                            s_brightness[i], (i == NUM_CHANNELS - 1) ? "" : ", ");
        }
        snprintf(json_response + len, sizeof(json_response) - len, "]}");
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);
    return ESP_OK;
}

// REST Endpoint Handler: GET /api/led_info
esp_err_t get_led_info_handler(httpd_req_t *req) {
    char json_response[256];
    int len = snprintf(json_response, sizeof(json_response), "{\"channels\": %d, \"gpios\": [", NUM_CHANNELS);
    for (int i = 0; i < NUM_CHANNELS; i++) {
        len += snprintf(json_response + len, sizeof(json_response) - len, "%d%s", 
                        LEDC_GPIOS[i], (i == NUM_CHANNELS - 1) ? "" : ", ");
    }
    snprintf(json_response + len, sizeof(json_response) - len, "]}");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);
    return ESP_OK;
}

// REST Endpoint Handler: GET /api/power?ch=0-5
esp_err_t get_power_handler(httpd_req_t *req) {
    char buf[64];
    int ch = -1;
    char json_response[256];

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "ch", param, sizeof(param)) == ESP_OK) {
            ch = atoi(param);
        }
    }

    if (ch >= 0 && ch < NUM_CHANNELS) {
        snprintf(json_response, sizeof(json_response), "{\"channel\": %d, \"power\": \"%s\"}", ch, s_led_on[ch] ? "on" : "off");
    } else {
        int len = snprintf(json_response, sizeof(json_response), "{\"power\": [");
        for (int i = 0; i < NUM_CHANNELS; i++) {
            len += snprintf(json_response + len, sizeof(json_response) - len, "\"%s\"%s", 
                            s_led_on[i] ? "on" : "off", (i == NUM_CHANNELS - 1) ? "" : ", ");
        }
        snprintf(json_response + len, sizeof(json_response) - len, "]}");
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);
    return ESP_OK;
}

// REST Endpoint Handler: POST /api/power?value=on|off&ch=0-5
esp_err_t set_power_handler(httpd_req_t *req) {
    char buf[64];
    int ch = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "ch", param, sizeof(param)) == ESP_OK) {
            ch = atoi(param);
        }
        if (ch >= 0 && ch < NUM_CHANNELS) {
            if (httpd_query_key_value(buf, "value", param, sizeof(param)) == ESP_OK) {
                if (strcmp(param, "on") == 0) {
                    s_led_on[ch] = true;
                } else if (strcmp(param, "off") == 0) {
                    s_led_on[ch] = false;
                } else {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid 'value' parameter. Use 'on' or 'off'.");
                    return ESP_FAIL;
                }
                ESP_LOGI(TAG, "LED Channel %d power set to %s", ch, s_led_on[ch] ? "ON" : "OFF");
                update_led_hardware(ch);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"status\":\"success\"}");
                return ESP_OK;
            }
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid parameters");
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

        httpd_uri_t favicon_uri = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &favicon_uri);

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
        ESP_LOGI(TAG, "Real URL for changing brightness: POST http://" IPSTR "/api/brightness?ch=0&value=50", 
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for getting brightness:  GET  http://" IPSTR "/api/brightness?ch=0", 
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for getting LED ports:   GET  http://" IPSTR "/api/led_info", 
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for changing power:      POST http://" IPSTR "/api/power?ch=0&value=on", 
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Real URL for getting power:       GET  http://" IPSTR "/api/power?ch=0", 
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
        ESP_LOGI(TAG, "connected to ap SSID:%s password:******",
                 CONFIG_ESP_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:******",
                 CONFIG_ESP_WIFI_SSID);
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