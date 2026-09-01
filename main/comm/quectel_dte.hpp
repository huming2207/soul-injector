//
// Created by hu on 1/9/26.
//

#ifndef SOULINJECTOR_QUECTEL_DTE_HPP
#define SOULINJECTOR_QUECTEL_DTE_HPP

#include <memory>
#include <string>
#include <driver/gpio.h>
#include <driver/uart.h>
#include "cxx_include/esp_modem_dte.hpp"
#include "esp_modem_config.h"

/**
 * DTE for Quectel modems
 * Pulls the DTR line low before every transaction to wake the modem up from its
 * sleep mode, and releases it back afterwards so the modem can sleep again.
 */
class QuectelDTE : public esp_modem::DTE
{
public:
    explicit QuectelDTE(
        const esp_modem_dte_config *config, std::unique_ptr<esp_modem::Terminal> terminal, uart_port_t port, gpio_num_t dtr = GPIO_NUM_NC
    ) : esp_modem::DTE(config, std::move(terminal)), dtr_pin(dtr), uart_port(port)
    {
    }
    ~QuectelDTE() override = default;

    /**
     * @brief Writing to the underlying terminal
     * @param data Data pointer to write
     * @param len Data len to write
     * @return number of bytes written
     */
    int write(uint8_t *data, size_t len) override;

    /**
     * @brief Sends an AT command and waits for the answer
     * @param command AT command
     * @param got_line callback if a line received
     * @param time_ms timeout in milliseconds
     * @return OK, FAIL or TIMEOUT
     */
    esp_modem::command_result command(const std::string &command, esp_modem::got_line_cb got_line, uint32_t time_ms) override;

    /**
     * @brief Sends an AT command and waits for the answer, with a specific line separator
     */
    esp_modem::command_result command(const std::string &command, esp_modem::got_line_cb got_line, uint32_t time_ms, char separator) override;

    static std::shared_ptr<QuectelDTE> create(const esp_modem_dte_config *config, gpio_num_t dtr);

private:
    void wake_modem();
    void allow_sleep();

private:
    gpio_num_t dtr_pin;
    uart_port_t uart_port;
};

#endif //SOULINJECTOR_QUECTEL_DTE_HPP
