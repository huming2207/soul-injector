//
// Created by hu on 1/9/26.
//

#ifndef SOULINJECTOR_PING_TEST_HPP
#define SOULINJECTOR_PING_TEST_HPP

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <apps/ping/ping_sock.h>

class ping_test
{
public:
    static ping_test *instance()
    {
        static ping_test _instance;
        return &_instance;
    }

    void operator=(ping_test const &) = delete;
    ping_test(ping_test const &) = delete;

private:
    ping_test() = default;

public:
    /**
     * Initialise the ping test
     * This spawns a task which waits for the modem to connect, then pings the
     * target through the modem's PPP netif every 30 seconds and logs the latency.
     * @return
     *   - ESP_OK when successful
     *   - ESP_ERR_NO_MEM when the task creation failed
     */
    esp_err_t init();

private:
    static void ping_task_func(void *_ctx);
    static void on_ping_success(esp_ping_handle_t hdl, void *args);
    static void on_ping_timeout(esp_ping_handle_t hdl, void *args);
    static void on_ping_end(esp_ping_handle_t hdl, void *args);

    void ping_task_handler();
    esp_err_t start_session(int netif_index);

private:
    bool inited = false;
    TaskHandle_t ping_task = nullptr;

private:
    static const constexpr char TAG[] = "ping_test";
    static const constexpr char PING_TARGET[] = "8.8.8.8";
    static const constexpr uint32_t PING_COUNT = 5;
    static const constexpr uint32_t PING_INTERVAL_MS = 30000;
};

#endif //SOULINJECTOR_PING_TEST_HPP
