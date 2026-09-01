//
// Created by hu on 1/9/26.
//

#include <cstdlib>
#include <cstring>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_ppp.h>
#include <driver/gpio.h>
#include "esp_modem_config.h"
#include "modem_manager.hpp"

esp_err_t modem_manager::init()
{
    if (inited) {
        return ESP_OK;
    }

    // Ignore errors here, the netif stack and the event loop may have been set up earlier
    esp_netif_init();
    esp_event_loop_create_default();

    // PWRKEY and RESET_N are pulled low through open-drain drivers, keep both released for now
    gpio_config_t ctrl_cfg = {};
    ctrl_cfg.pin_bit_mask = (1ULL << CELL_PWRKEY_PIN) | (1ULL << CELL_RST_PIN);
    ctrl_cfg.mode = GPIO_MODE_OUTPUT;
    ctrl_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    ctrl_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ctrl_cfg.intr_type = GPIO_INTR_DISABLE;
    auto ret = gpio_config(&ctrl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Can't config the modem's control pins: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    gpio_set_level(CELL_PWRKEY_PIN, 0);
    gpio_set_level(CELL_RST_PIN, 0);

    // Hold DTR low so the modem won't enter sleep mode
    gpio_reset_pin(CELL_DTR_PIN);
    gpio_set_direction(CELL_DTR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CELL_DTR_PIN, 0);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    netif = esp_netif_new(&netif_cfg);
    if (netif == nullptr) {
        ESP_LOGE(TAG, "Can't create the PPP netif");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, this);
    ret = ret ?: esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &ppp_event_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Can't register the modem's events: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    evt_group = xEventGroupCreate();
    if (evt_group == nullptr) {
        ESP_LOGE(TAG, "Can't create the event group");
        return ESP_ERR_NO_MEM;
    }

    auto task_ret = xTaskCreate(modem_task_func, "modem_mgr", 6144, this, 5, &modem_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Can't create the modem task");
        return ESP_ERR_NO_MEM;
    }

    inited = true;
    ESP_LOGI(TAG, "Modem manager init OK");
    return ESP_OK;
}

bool modem_manager::is_connected()
{
    return evt_group != nullptr && (xEventGroupGetBits(evt_group) & BIT_CONNECTED) != 0;
}

esp_err_t modem_manager::wait_for_connect(uint32_t timeout_ticks)
{
    if (evt_group == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    return (xEventGroupWaitBits(evt_group, BIT_CONNECTED, pdFALSE, pdTRUE, timeout_ticks) & BIT_CONNECTED) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

void modem_manager::set_got_ip_cb(const std::function<void(esp_netif_ip_info_t *)> &cb)
{
    got_ip_cb = cb;
}

void modem_manager::set_lost_ip_cb(const std::function<void()> &cb)
{
    lost_ip_cb = cb;
}

void modem_manager::modem_task_func(void *_ctx)
{
    auto *ctx = static_cast<modem_manager *>(_ctx);
    ctx->modem_task_handler();
}

void modem_manager::modem_task_handler()
{
    // The modem may already be running (e.g. after a soft reboot), in that case the first setup attempt
    // just succeeds and the power key is never touched. Pressing the power key on a running modem would
    // power it off instead!
    esp_err_t ret = setup_modem();
    if (ret != ESP_OK) {
        power_on();
        ret = setup_modem();
    }

    while (true) {
        while (ret != ESP_OK) {
            ESP_LOGW(TAG, "Modem setup failed (0x%x), resetting the modem", ret);
            vTaskDelay(pdMS_TO_TICKS(SETUP_RETRY_DELAY_MS));
            hardware_reset();
            ret = setup_modem();
        }

        // Keep the PPP link alive, every drop gets a re-dial. When the modem stops answering properly,
        // fall back to a full hardware reset and start over.
        while (true) {
            ret = wait_for_registration();
            if (ret != ESP_OK) {
                break;
            }

            ret = dial_out();
            if (ret != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(DIAL_RETRY_DELAY_MS));
                continue;
            }

            // Connected, now just wait till the link drops, then re-dial
            xEventGroupWaitBits(evt_group, BIT_LINK_DOWN, pdTRUE, pdFALSE, portMAX_DELAY);
            ESP_LOGW(TAG, "PPP link down, re-dialing");
        }
    }
}

void modem_manager::power_on()
{
    ESP_LOGI(TAG, "Powering the modem on");

    gpio_set_level(CELL_RST_PIN, 1); // Assert the reset
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(CELL_PWRKEY_PIN, 1); // Press the power key
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(CELL_RST_PIN, 0);    // Release the reset
    vTaskDelay(pdMS_TO_TICKS(610));     // The power key must be pressed for at least one second
    gpio_set_level(CELL_PWRKEY_PIN, 0); // Release the power key

    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_DELAY_MS)); // Wait till the modem boots up
}

void modem_manager::hardware_reset()
{
    ESP_LOGW(TAG, "Resetting the modem");
    dispose_modem();

    gpio_set_level(CELL_RST_PIN, 1); // Assert the reset
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(CELL_RST_PIN, 0); // Release the reset

    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_DELAY_MS)); // Wait till the modem boots up
}

void modem_manager::dispose_modem()
{
    dce.reset();
    dte.reset();
    if (uart_is_driver_installed(CELL_UART_PORT)) {
        uart_driver_delete(CELL_UART_PORT);
    }
}

esp_err_t modem_manager::setup_modem()
{
    dispose_modem();

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.port_num = CELL_UART_PORT;
    dte_config.uart_config.tx_io_num = CELL_TX_PIN;
    dte_config.uart_config.rx_io_num = CELL_RX_PIN;
    dte_config.uart_config.rts_io_num = CELL_RTS_PIN;
    dte_config.uart_config.cts_io_num = CELL_CTS_PIN;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_HW;
    dte_config.uart_config.baud_rate = CELL_BAUD_RATE;
    dte_config.uart_config.source_clk = UART_SCLK_XTAL;

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CELL_APN);

    dte = esp_modem::create_uart_dte(&dte_config);
    if (dte == nullptr) {
        ESP_LOGE(TAG, "Can't create the UART DTE");
        return ESP_FAIL;
    }

    dce = esp_modem::dce_factory::Factory::build_unique<esp_modem::GenericModule>(&dce_config, std::move(dte), netif);
    if (dce == nullptr) {
        ESP_LOGE(TAG, "Can't create the modem DCE");
        return ESP_FAIL;
    }

    // Wait for the modem to answer AT commands, it may still be booting up
    esp_modem::command_result res = esp_modem::command_result::TIMEOUT;
    for (uint32_t attempt = 0; attempt < SYNC_RETRY_ATTEMPTS; attempt++) {
        res = dce->sync();
        if (res == esp_modem::command_result::OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SYNC_RETRY_DELAY_MS));
    }

    if (res != esp_modem::command_result::OK) {
        ESP_LOGE(TAG, "The modem doesn't respond to AT");
        return ESP_ERR_TIMEOUT;
    }

    // Match the modem's flow control with the UART's hardware flow control
    if (dce->set_flow_control(2, 2) != esp_modem::command_result::OK) {
        ESP_LOGE(TAG, "Can't set the modem's flow control");
        return ESP_FAIL;
    }

    std::string imei = {};
    if (dce->get_imei(imei) == esp_modem::command_result::OK) {
        ESP_LOGI(TAG, "Modem IMEI: %s", imei.c_str());
    }

    return ESP_OK;
}

esp_err_t modem_manager::wait_for_registration()
{
    std::string out = {};
    int32_t reg_stat = -1;
    uint32_t read_fail_cnt = 0;

    for (uint32_t attempt = 0; attempt < REG_MAX_ATTEMPTS; attempt++) {
        out.clear();
        if (dce->at("AT+CEREG?", out, 300) != esp_modem::command_result::OK) {
            // A few failed reads in a row mean the modem is in a bad shape
            if (++read_fail_cnt >= REG_MAX_READ_FAILS) {
                ESP_LOGE(TAG, "Can't read the network registration status");
                return ESP_FAIL;
            }
        } else {
            read_fail_cnt = 0;
            reg_stat = parse_cereg(out);
            if (reg_stat == 1 || reg_stat == 5) {
                ESP_LOGI(TAG, "Registered on the network (stat %ld)", (long)reg_stat);
                return ESP_OK;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(REG_POLL_DELAY_MS));
    }

    ESP_LOGE(TAG, "Network registration timed out");
    return ESP_ERR_TIMEOUT;
}

esp_err_t modem_manager::dial_out()
{
    // Always dial out from command mode
    if (dce->get_mode() != esp_modem::modem_mode::COMMAND_MODE) {
        if (!dce->set_mode(esp_modem::modem_mode::COMMAND_MODE)) {
            ESP_LOGE(TAG, "Can't return to command mode");
            return ESP_ERR_INVALID_STATE;
        }
    }

    xEventGroupClearBits(evt_group, BIT_CONNECTED);

    // Dial out and switch the DTE to the PPP data mode
    esp_err_t ret = ESP_FAIL;
    for (uint32_t attempt = 0; attempt < DIAL_RETRY_ATTEMPTS; attempt++) {
        if (dce->set_mode(esp_modem::modem_mode::DATA_MODE)) {
            ret = ESP_OK;
            break;
        }

        ESP_LOGW(TAG, "Dial out failed (attempt %lu)", (unsigned long)(attempt + 1));
        vTaskDelay(pdMS_TO_TICKS(DIAL_RETRY_DELAY_MS));
    }

    if (ret != ESP_OK) {
        return ret;
    }

    // Wait for the IP assignment
    auto bits = xEventGroupWaitBits(evt_group, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(IP_WAIT_TIMEOUT_MS));
    if ((bits & BIT_CONNECTED) == 0) {
        ESP_LOGE(TAG, "Didn't get an IP address in time");
        dce->set_mode(esp_modem::modem_mode::COMMAND_MODE); // Best effort recovery for the next attempt
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

int32_t modem_manager::parse_cereg(const std::string &out)
{
    // Expected format: "+CEREG: <n>,<stat>[,...]"
    const char *prefix = strstr(out.c_str(), "+CEREG: ");
    if (prefix == nullptr) {
        return -1;
    }

    const char *p = prefix + 8;
    char *endptr = nullptr;

    strtol(p, &endptr, 10); // Skip the <n> value
    if (endptr == p || *endptr != ',') {
        return -1;
    }

    p = endptr + 1;
    const long stat = strtol(p, &endptr, 10);
    if (endptr == p) {
        return -1;
    }

    return (int32_t)stat;
}

void modem_manager::ip_event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data)
{
    auto *ctx = static_cast<modem_manager *>(_ctx);
    if (ctx == nullptr) {
        return;
    }

    if (evt_id == IP_EVENT_PPP_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)evt_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(ctx->evt_group, BIT_CONNECTED);
        if (ctx->got_ip_cb) {
            ctx->got_ip_cb(&event->ip_info);
        }
    } else if (evt_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP!");
        xEventGroupClearBits(ctx->evt_group, BIT_CONNECTED);
        xEventGroupSetBits(ctx->evt_group, BIT_LINK_DOWN);
        if (ctx->lost_ip_cb) {
            ctx->lost_ip_cb();
        }
    }
}

void modem_manager::ppp_event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data)
{
    auto *ctx = static_cast<modem_manager *>(_ctx);
    if (ctx == nullptr) {
        return;
    }

    if (evt_id > NETIF_PPP_ERRORNONE && evt_id <= NETIF_PPP_ERRORLOOPBACK) {
        ESP_LOGW(TAG, "Got PPP error 0x%lx", (unsigned long)evt_id);
        xEventGroupClearBits(ctx->evt_group, BIT_CONNECTED);
        xEventGroupSetBits(ctx->evt_group, BIT_LINK_DOWN);
        if (ctx->lost_ip_cb) {
            ctx->lost_ip_cb();
        }
    }
}
