#pragma once

#include <cstdint>
#include <cstddef>

// What the screen shows, as plain data. Built by ui_screens, drawn by ui_view.
// Neither depends on ESP-IDF, so tools/ui_preview renders the same screens on
// a computer.
struct ui_model
{
    enum class tone : uint8_t
    {
        neutral, // black: ready, working, debugging
        pass,    // green
        fail,    // red: the run failed
        notice,  // amber: the device needs attention, not the board
    };

    tone colour = tone::neutral;
    bool big_title = false; // pass and fail are read from across the bench
    int8_t progress = -1;   // 0 to 100, or -1 for no bar
    char header[24] = {};   // activity, in capitals: READY, PROGRAMMING, ...
    char title[40] = {};
    char detail[160] = {};

    bool operator==(const ui_model &other) const;
};

namespace ui_screens
{
    ui_model booting();
    ui_model ready(bool watching);
    ui_model starting();
    ui_model connecting();
    ui_model erasing(int progress);
    ui_model writing(int progress);
    ui_model checking();
    ui_model finishing();
    ui_model testing(size_t done, size_t total, const char *what);
    ui_model passed();
    ui_model failed(const char *title, const char *detail);
    ui_model notice(const char *title, const char *detail);
    ui_model setup();
    ui_model current(double min_ua, double max_ua, double avg_ua, const char *result);

    enum class debug_state : uint8_t
    {
        none,    // no debugger attached
        stopped, // the target is halted
        running,
        lost,    // the target stopped answering
    };
    ui_model debugging(debug_state state);

    // Shown at the right of the header.
    enum class link : uint8_t
    {
        none,
        usb,
        network,
    };
    const char *link_text(link l);
}
