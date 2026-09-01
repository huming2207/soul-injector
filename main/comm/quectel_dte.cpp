//
// Created by hu on 1/9/26.
//

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "uart_terminal.hpp"
#include "quectel_dte.hpp"

int QuectelDTE::write(uint8_t *data, size_t len)
{
    wake_modem();
    const auto ret = DTE::write(data, len);
    allow_sleep();
    return ret;
}

esp_modem::command_result QuectelDTE::command(const std::string &command, esp_modem::got_line_cb got_line, uint32_t time_ms)
{
    wake_modem();
    const auto res = DTE::command(command, std::move(got_line), time_ms);
    allow_sleep();
    return res;
}

esp_modem::command_result QuectelDTE::command(const std::string &command, esp_modem::got_line_cb got_line, uint32_t time_ms, char separator)
{
    wake_modem();
    const auto res = DTE::command(command, std::move(got_line), time_ms, separator);
    allow_sleep();
    return res;
}

void QuectelDTE::wake_modem()
{
    if (dtr_pin == GPIO_NUM_NC) {
        return;
    }

    gpio_set_level(dtr_pin, 0); // Low for waking the modem up
    vTaskDelay(1);
}

void QuectelDTE::allow_sleep()
{
    if (dtr_pin == GPIO_NUM_NC) {
        return;
    }

    uart_wait_tx_done(uart_port, portMAX_DELAY);
    gpio_set_level(dtr_pin, 1); // High for letting the modem sleep again
}

std::shared_ptr<QuectelDTE> QuectelDTE::create(const esp_modem_dte_config *config, gpio_num_t dtr)
{
    auto terminal = esp_modem::create_uart_terminal(config);
    if (terminal == nullptr) {
        return nullptr;
    }

    return std::make_shared<QuectelDTE>(config, std::move(terminal), config->uart_config.port_num, dtr);
}
