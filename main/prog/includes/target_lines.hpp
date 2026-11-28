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

    /**
     * Connect, read the DP IDCODE (no reset, no halt), isolate. Holds the CPU
     * at full speed meanwhile, which also keeps light sleep from isolating the
     * pins mid-probe. The caller must own the target (a job_controller claim).
     */
    static esp_err_t probe(uint32_t *idcode);

    /** Drive the target's nRST line (asserted: held in reset). */
    static void drive_nrst(bool asserted);

    /**
     * Early in boot, for a trap armed with --connect-under-reset: hold the
     * target in reset, lines connected, until the trap attaches under reset
     * (swd_connect_under_reset() lets it go) or end_boot_hold() is called.
     */
    static void hold_reset_from_boot();
    /**
     * Ends a hold from boot, if there is one: nRST released and the lines
     * isolated. Called before anything else takes the target (a programming
     * run, a plain attach) and when the trap's first start doesn't attach.
     * Any task; only the first call after a hold does anything.
     */
    static void end_boot_hold();

private:
    static void set_swdio_pulldowns(bool enable);
};
