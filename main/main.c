
#include "ws2812.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <mqtt_client.h>
#include <nvs_flash.h>

static char const TAG[] = "main";

static esp_mqtt_client_handle_t mqtt_handle;

static uint8_t led_rgb[3 * CONFIG_LED_COUNT];

bool handle_message(void const *data, int len) {
    cJSON *json = cJSON_ParseWithLength(data, len);
    if (!json) {
        return false;
    }

    // Get LED index.
    int    led_index = 0;
    cJSON *led_obj   = cJSON_GetObjectItemCaseSensitive(json, "led_index");
    if (led_obj) {
        if (!cJSON_IsNumber(led_obj)) {
            ESP_LOGE(TAG, "`led_index` should be an integer");
            return false;
        }
        double tmp = cJSON_GetNumberValue(led_obj);
        led_index  = tmp;
        if (led_index != tmp) {
            return false;
        }
        if (led_index < 0 || led_index >= CONFIG_LED_COUNT) {
            ESP_LOGE(TAG, "`led_index` out of range");
            return false;
        }
    }

    // Get RGB values list.
    cJSON *val_obj = cJSON_GetObjectItemCaseSensitive(json, "led_data");
    if (!val_obj) {
        ESP_LOGE(TAG, "Missing `led_data`");
        return false;
    } else if (!cJSON_IsArray(val_obj)) {
        ESP_LOGE(TAG, "`led_data` must be an array of integers");
        return false;
    }
    int val_len = cJSON_GetArraySize(val_obj);
    if (val_len <= 0 || val_len % 3) {
        ESP_LOGE(TAG, "Invalid `led_data` size");
        return false;
    }
    val_len /= 3;
    if (led_index + val_len < val_len || led_index + val_len >= CONFIG_LED_COUNT) {
        ESP_LOGE(TAG, "Too many LEDs in `led_data`");
        return false;
    }

    // Convert into list of integers.
    uint8_t *rgb_tmp = malloc(val_len * 3);
    for (int i = 0; i < val_len * 3; i++) {
        cJSON *col_obj = cJSON_GetArrayItem(val_obj, i);
        if (!col_obj || !cJSON_IsNumber(col_obj)) {
            ESP_LOGE(TAG, "`led_data` must be an array of integers");
            free(rgb_tmp);
            return false;
        }
        double tmp = cJSON_GetNumberValue(col_obj);
        int    col = tmp;
        if (col != tmp) {
            ESP_LOGE(TAG, "`led_data` must be an array of integers");
            free(rgb_tmp);
            return false;
        } else if (col < 0 || col > 255) {
            ESP_LOGE(TAG, "LED data out of range [0,255]");
            free(rgb_tmp);
            return false;
        }
        rgb_tmp[i] = col;
    }

    // Copy values into WS2812 data.
    memcpy(led_rgb + 3 * led_index, rgb_tmp, 3 * val_len);
    free(rgb_tmp);

    // Update the LEDs.
    ESP_ERROR_CHECK_WITHOUT_ABORT(ws2812_send_data(led_rgb, CONFIG_LED_COUNT));

    return true;
}

void mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *_event_data) {
    (void)arg;
    esp_mqtt_event_t *event_data = _event_data;
    if (event_id == MQTT_EVENT_DATA) {
        if (!handle_message(event_data->data, event_data->data_len)) {
            ESP_LOGE(
                TAG,
                "Bad MQTT message: %.*s: %.*s",
                event_data->topic_len,
                event_data->topic,
                event_data->data_len,
                event_data->data
            );
        } else {
            ESP_LOGI(
                TAG,
                "Good MQTT message: %.*s: %.*s",
                event_data->topic_len,
                event_data->topic,
                event_data->data_len,
                event_data->data
            );
        }
    } else if (event_id == MQTT_EVENT_SUBSCRIBED) {
        ESP_LOGI(TAG, "MQTT subscribed, id=%d", event_data->msg_id);
    } else if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT connected");
        int res = esp_mqtt_client_subscribe_single(mqtt_handle, CONFIG_MQTT_TOPIC, 2);
        if (res < 0) {
            ESP_LOGE(TAG, "Failed to subscribe to %s", CONFIG_MQTT_TOPIC);
        }
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "MQTT disconnected");
    }
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        esp_wifi_connect();
    }
}

void app_main() {
    ESP_ERROR_CHECK(ws2812_init(CONFIG_LED_GPIO, CONFIG_LED_COUNT * 3));

    // Init NVS, which is required for WiFi.
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    // Init and connect to WiFi.
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    wifi_config_t wifi_conf = {
        .sta = {
            .ssid      = CONFIG_WIFI_SSID,
            .password  = CONFIG_WIFI_PASS,
            .threshold = {
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_conf));
    ESP_ERROR_CHECK(esp_wifi_start());

    vTaskDelay(pdMS_TO_TICKS(2000));

    // Init MQTT client.
    esp_mqtt_client_config_t mqtt_conf = {
        .broker.address.uri = CONFIG_MQTT_SERVER,
    };
    mqtt_handle = esp_mqtt_client_init(&mqtt_conf);
    esp_mqtt_client_register_event(mqtt_handle, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_handle);
}
