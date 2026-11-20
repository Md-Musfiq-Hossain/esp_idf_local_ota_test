#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "lwip/sockets.h"

#define WIFI_SSID      ""
#define WIFI_PASS      ""
#define MDNS_HOSTNAME  "esp32"
#define LOG_TCP_PORT   3333

static const char *TAG = "WIFI_DEV";
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static int client_sock = -1;
static vprintf_like_t default_vprintf = NULL;

/* ============================================================
 * 1. WIRELESS SERIAL MONITOR (TCP LOG REDIRECTOR)
 * ============================================================ */
int wifi_log_vprintf(const char *fmt, va_list args) {
    // Also print to USB UART if connected
    va_list args_copy;
    va_copy(args_copy, args);
    int ret = default_vprintf(fmt, args_copy);
    va_end(args_copy);

    // Send over TCP socket if a client is listening
    if (client_sock >= 0) {
        char buf[512];
        int len = vsnprintf(buf, sizeof(buf), fmt, args);
        if (len > 0) {
            int sent = send(client_sock, buf, MIN(len, sizeof(buf) - 1), 0);
            if (sent < 0) {
                close(client_sock);
                client_sock = -1;
            }
        }
    }
    return ret;
}

void tcp_log_server_task(void *pvParameters) {
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(LOG_TCP_PORT)
    };

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(listen_sock, 1);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int new_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (new_sock >= 0) {
            if (client_sock >= 0) {
                close(client_sock);
            }
            client_sock = new_sock;
            const char *welcome = "--- Connected to ESP32 Wireless Monitor ---\r\n";
            send(client_sock, welcome, strlen(welcome), 0);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ============================================================
 * 2. WIRELESS CODE UPLOAD (HTTP POST /update HANDLER)
 * ============================================================ */
esp_err_t ota_post_handler(httpd_req_t *req) {
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    
    if (update_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting OTA update to partition: %s", update_partition->label);
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int received;
    int remaining = req->content_len;

    while (remaining > 0) {
        received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota_handle, buf, received) != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    if (esp_ota_end(ota_handle) != ESP_OK || esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation/boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA Success! Rebooting in 1 second...");
    httpd_resp_sendstr(req, "OTA Update Successful! Rebooting...\n");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

void start_ota_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t ota_uri = {
            .uri       = "/update",
            .method    = HTTP_POST,
            .handler   = ota_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &ota_uri);
        ESP_LOGI(TAG, "HTTP OTA Server ready at http://%s.local/update", MDNS_HOSTNAME);
    }
}

/* ============================================================
 * 3. WI-FI & mDNS INITIALIZATION
 * ============================================================ */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void init_wifi_and_mdns(void) {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    // Initialize mDNS so esp32.local works
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
}

/* ============================================================
 * MAIN APPLICATION ENTRY
 * ============================================================ */
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1. Connect to Wi-Fi & Start mDNS
    init_wifi_and_mdns();

    // 2. Start Wireless Log Monitor
    default_vprintf = esp_log_set_vprintf(wifi_log_vprintf);
    xTaskCreate(tcp_log_server_task, "tcp_log_server", 4096, NULL, 5, NULL);

    // 3. Start Wireless OTA Server
    start_ota_webserver();

    // Your main application loop goes below!
    int counter = 0;
    while (1) {
        ESP_LOGI(TAG, "Running wirelessly! Loop count: %d", counter++);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
