#pragma once

#include <ui/ui_if.hpp>

// The firmware's screen API: ui_composer plus the steps and notices the
// 2.79" UI shows. Every call returns at once from any task: none waits for
// the display, so programming and debugging never slow down for it.
class si_ui : public ui_composer
{
public:
    // A run has started: the job is being loaded and prepared.
    virtual esp_err_t display_starting() = 0;
    // The run is connecting to the board.
    virtual esp_err_t display_connecting() = 0;
    // The written firmware is being read back.
    virtual esp_err_t display_verify() = 0;
    // The device, not the board, needs attention: no job, log full, ...
    virtual esp_err_t display_notice(const char *title, const char *detail) = 0;
    // Progress of the step on screen; cheap enough to call for every block written.
    virtual void set_progress(uint32_t done, uint32_t total) = 0;
};
