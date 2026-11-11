#pragma once

#include <esp_err.h>

/**
 * The analog switch between the SWD/UART front end and the programming
 * connector. It isolates the target unless something is using it, so current
 * can be measured with the cable still attached.
 *
 * Only a programming run or a debug session may use it, and never both at once.
 * Boards without the switch (CONFIG_SI_TARGET_SWITCH_PIN = -1) ignore these calls.
 */
class target_lines
{
public:
    /** Configure the pin and leave the target isolated. */
    static esp_err_t init();

    static void connect();
    static void isolate();

private:
    static void set_swdio_pulldowns(bool enable);
};
