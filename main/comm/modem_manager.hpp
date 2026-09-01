//
// Created by hu on 1/9/26.
//

#ifndef SOULINJECTOR_MODEM_MANAGER_HPP
#define SOULINJECTOR_MODEM_MANAGER_HPP

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <functional>
#include <memory>
#include <string>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <driver/uart.h>
#include <soc/gpio_num.h>
#include "cxx_include/esp_modem_api.hpp"
#include "cxx_include/esp_modem_dce_factory.hpp"

class modem_manager
{
public:
    enum evt_bits : uint32_t {
        BIT_CONNECTED = (1UL << 0UL), // Set when the modem got an IP address, i.e. the PPP link is up
        BIT_LINK_DOWN = (1UL << 1UL), // Set when the PPP link was lost and needs a re-dial
    };

public:
    static modem_manager *instance()
    {
        static modem_manager _instance;
        return &_instance;
    }

    void operator=(modem_manager const &) = delete;
    modem_manager(modem_manager const &) = delete;

private:
    modem_manager() = default;

public:
    /**
     * Initialise the modem manager
     * This configures the modem's control GPIOs, brings up the PPP netif and starts the modem task,
     * which powers on the modem and dials out to the network.
     * @return
     *   - ESP_OK when successful
     *   - Other errors when GPIO or netif setup failed
     */
    esp_err_t init();

    /**
     * Query whether the modem is connected to the network (got an IP address) or not
     * @return True if connected, false otherwise
     */
    bool is_connected();

    /**
     * Wait for the modem to connect to the network
     * @param timeout_ticks Timeout in RTOS ticks
     * @return ESP_OK if connected, ESP_ERR_TIMEOUT if timed out
     */
    esp_err_t wait_for_connect(uint32_t timeout_ticks);

    void set_got_ip_cb(const std::function<void(esp_netif_ip_info_t *)> &cb);
    void set_lost_ip_cb(const std::function<void()> &cb);

private:
    static void modem_task_func(void *_ctx);
    static void ip_event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data);
    static void ppp_event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data);

    void modem_task_handler();
    void power_on();
    void hardware_reset();
    void dispose_modem();
    esp_err_t setup_modem();
    esp_err_t wait_for_registration();
    esp_err_t dial_out();
    static int32_t parse_cereg(const std::string &out);

private:
    bool inited = false;
    EventGroupHandle_t evt_group = nullptr;
    esp_netif_t *netif = nullptr;
    TaskHandle_t modem_task = nullptr;
    std::unique_ptr<esp_modem::DCE> dce = nullptr;
    std::shared_ptr<esp_modem::DTE> dte = {};
    std::function<void()> lost_ip_cb = {};
    std::function<void(esp_netif_ip_info_t *)> got_ip_cb = {};

private:
    static const constexpr char TAG[] = "modem_mgr";

    // Modem's main UART
    static const constexpr uart_port_t CELL_UART_PORT = UART_NUM_1;
    static const constexpr int CELL_TX_PIN = 47; // ESP32's TXD, goes to the modem's RXD
    static const constexpr int CELL_RX_PIN = 46; // ESP32's RXD, comes from the modem's TXD
    static const constexpr int CELL_RTS_PIN = 42;
    static const constexpr int CELL_CTS_PIN = 43;
    static const constexpr int CELL_BAUD_RATE = 115200;

    // Modem's control lines, PWRKEY and RESET_N are pulled low through open-drain drivers,
    // DTR goes through the level shifter as is
    static const constexpr gpio_num_t CELL_PWRKEY_PIN = GPIO_NUM_49;
    static const constexpr gpio_num_t CELL_RST_PIN = GPIO_NUM_48;
    static const constexpr gpio_num_t CELL_DTR_PIN = GPIO_NUM_45;

    // TODO: hardcoded APN for now
    static const constexpr char CELL_APN[] = "quectel.st.std";

    // Time for the modem to boot up after a power on or a hardware reset
    static const constexpr uint32_t MODEM_BOOT_DELAY_MS = 9500;
    // Sync (AT) attempts before the modem is considered dead
    static const constexpr uint32_t SYNC_RETRY_ATTEMPTS = 20;
    static const constexpr uint32_t SYNC_RETRY_DELAY_MS = 500;
    // Network registration poll, registration can legitimately take a while on LTE
    static const constexpr uint32_t REG_MAX_ATTEMPTS = 450;
    static const constexpr uint32_t REG_POLL_DELAY_MS = 2000;
    static const constexpr uint32_t REG_MAX_READ_FAILS = 10;
    // Dial out retries and the wait for the IP assignment
    static const constexpr uint32_t DIAL_RETRY_ATTEMPTS = 3;
    static const constexpr uint32_t DIAL_RETRY_DELAY_MS = 3000;
    static const constexpr uint32_t IP_WAIT_TIMEOUT_MS = 60000;
    static const constexpr uint32_t SETUP_RETRY_DELAY_MS = 3000;
};

#endif //SOULINJECTOR_MODEM_MANAGER_HPP
