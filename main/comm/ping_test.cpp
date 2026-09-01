//
// Created by hu on 1/9/26.
//

#include <inttypes.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include "modem_manager.hpp"
#include "ping_test.hpp"

esp_err_t ping_test::init()
{
    if (inited) {
        return ESP_OK;
    }

    auto task_ret = xTaskCreate(ping_task_func, "ping_test", 4096, this, 5, &ping_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Can't create the ping test task");
        return ESP_ERR_NO_MEM;
    }

    inited = true;
    return ESP_OK;
}

void ping_test::ping_task_func(void *_ctx)
{
    auto *ctx = static_cast<ping_test *>(_ctx);
    ctx->ping_task_handler();
}

void ping_test::ping_task_handler()
{
    auto *modem = modem_manager::instance();

    // The pings need the modem's PPP link up first
    if (modem->wait_for_connect(portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "The modem manager isn't initialised, ping test stops");
        vTaskDelete(nullptr);
        return;
    }

    esp_netif_t *netif = modem->get_netif();
    if (netif == nullptr) {
        ESP_LOGE(TAG, "No netif to ping through, ping test stops");
        vTaskDelete(nullptr);
        return;
    }

    const int netif_index = esp_netif_get_netif_impl_index(netif);
    ESP_LOGI(TAG, "Pinging %s every %lu seconds", PING_TARGET, (unsigned long)(PING_INTERVAL_MS / 1000));

    while (true) {
        const TickType_t cycle_start = xTaskGetTickCount();

        if (start_session(netif_index) == ESP_OK) {
            // Wait for the session to end, the end callback notifies us
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
            continue;
        }

        // Keep a 30 second period between the test rounds
        const TickType_t elapsed = xTaskGetTickCount() - cycle_start;
        if (elapsed < pdMS_TO_TICKS(PING_INTERVAL_MS)) {
            vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS) - elapsed);
        }
    }
}

esp_err_t ping_test::start_session(int netif_index)
{
    ip_addr_t target = {};
    if (!ipaddr_aton(PING_TARGET, &target)) {
        ESP_LOGE(TAG, "Can't parse the ping target");
        return ESP_ERR_INVALID_ARG;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = PING_COUNT;
    config.target_addr = target;
    config.interface = netif_index;

    esp_ping_callbacks_t callbacks = {};
    callbacks.cb_args = this;
    callbacks.on_ping_success = on_ping_success;
    callbacks.on_ping_timeout = on_ping_timeout;
    callbacks.on_ping_end = on_ping_end;

    esp_ping_handle_t ping = nullptr;
    auto ret = esp_ping_new_session(&config, &callbacks, &ping);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Can't create the ping session: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ping_start(ping);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Can't start the ping session: 0x%x %s", ret, esp_err_to_name(ret));
        esp_ping_delete_session(ping);
    }
    return ret;
}

void ping_test::on_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t sequence = 0;
    uint32_t elapsed_time_ms = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &sequence, sizeof(sequence));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time_ms, sizeof(elapsed_time_ms));
    ESP_LOGI(TAG, "Ping reply: seq=%" PRIu16 ", time=%" PRIu32 " ms", sequence, elapsed_time_ms);
}

void ping_test::on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t sequence = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &sequence, sizeof(sequence));
    ESP_LOGW(TAG, "Ping timeout: seq=%" PRIu16, sequence);
}

void ping_test::on_ping_end(esp_ping_handle_t hdl, void *args)
{
    auto *ctx = static_cast<ping_test *>(args);
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration_ms = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &duration_ms, sizeof(duration_ms));
    ESP_LOGI(TAG, "Ping done: %" PRIu32 "/%" PRIu32 " replies in %" PRIu32 " ms", received, transmitted, duration_ms);

    esp_ping_delete_session(hdl);
    if (ctx != nullptr && ctx->ping_task != nullptr) {
        xTaskNotifyGive(ctx->ping_task);
    }
}
