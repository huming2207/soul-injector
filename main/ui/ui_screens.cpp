#include <cstdio>
#include <cstring>

#include "ui_model.hpp"

// Text is for factory staff first: short, plain English, and saying what to do next.

bool ui_model::operator==(const ui_model &other) const
{
    return colour == other.colour && big_title == other.big_title && progress == other.progress &&
           strcmp(header, other.header) == 0 && strcmp(title, other.title) == 0 && strcmp(detail, other.detail) == 0;
}

namespace
{
    template <size_t N>
    void copy(char (&dst)[N], const char *src)
    {
        snprintf(dst, N, "%s", src != nullptr ? src : "");
    }

    ui_model make(ui_model::tone colour, const char *header, const char *title, const char *detail, int progress = -1)
    {
        ui_model m = {};
        m.colour = colour;
        m.big_title = colour == ui_model::tone::pass || colour == ui_model::tone::fail;
        m.progress = progress < 0 ? -1 : (progress > 100 ? 100 : static_cast<int8_t>(progress));
        copy(m.header, header);
        copy(m.title, title);
        copy(m.detail, detail);
        return m;
    }
} // namespace

namespace ui_screens
{
    ui_model booting()
    {
        return make(ui_model::tone::neutral, "SOUL INJECTOR", "Starting up", "Please wait");
    }

    ui_model ready(bool watching)
    {
        return make(ui_model::tone::neutral, "READY", "Ready", watching ? "Watching the board for errors" : "Waiting for a board");
    }

    ui_model starting()
    {
        return make(ui_model::tone::neutral, "PROGRAMMING", "Starting", "Getting the job ready");
    }

    ui_model connecting()
    {
        return make(ui_model::tone::neutral, "PROGRAMMING", "Connecting", "Looking for the board");
    }

    ui_model erasing(int progress)
    {
        return make(ui_model::tone::neutral, "PROGRAMMING", "Erasing", "Removing the old firmware",
                    progress);
    }

    ui_model writing(int progress)
    {
        return make(ui_model::tone::neutral, "PROGRAMMING", "Writing", "Writing the new firmware", progress);
    }

    ui_model checking()
    {
        return make(ui_model::tone::neutral, "PROGRAMMING", "Checking", "Reading the firmware back");
    }

    ui_model finishing()
    {
        return make(ui_model::tone::neutral, "PROGRAMMING", "Finishing", "Running the job's last steps");
    }

    ui_model testing(size_t done, size_t total, const char *what)
    {
        char detail[64] = {};
        if (what != nullptr) {
            snprintf(detail, sizeof(detail), "%s", what);
        } else {
            snprintf(detail, sizeof(detail), "Test %u of %u", static_cast<unsigned>(done + 1), static_cast<unsigned>(total + 1));
        }
        // The job's tests count from 0 to total; a script test has no count.
        int progress = what == nullptr ? static_cast<int>(done * 100 / (total + 1)) : -1;
        return make(ui_model::tone::neutral, "PROGRAMMING", "Testing", detail, progress);
    }

    ui_model passed()
    {
        return make(ui_model::tone::pass, "PASSED", "Pass", "Done. Connect the next board.");
    }

    ui_model failed(const char *title, const char *detail)
    {
        return make(ui_model::tone::fail, "FAILED", title, detail);
    }

    ui_model notice(const char *title, const char *detail)
    {
        return make(ui_model::tone::notice, "ATTENTION", title, detail);
    }

    ui_model setup()
    {
        return make(ui_model::tone::neutral, "SETUP", "Connect a computer", "Plug in a USB cable to set up this device");
    }

    ui_model current(double min_ua, double max_ua, double avg_ua, const char *result)
    {
        char title[40] = {};
        char detail[96] = {};
        snprintf(title, sizeof(title), "%.1f uA average", avg_ua);
        snprintf(detail, sizeof(detail), "Lowest %.1f uA, highest %.1f uA\nResult: %s", min_ua, max_ua, result != nullptr ? result : "");
        return make(ui_model::tone::neutral, "CURRENT TEST", title, detail);
    }

    ui_model debugging(debug_state state)
    {
        const char *title = "Debugging";
        switch (state) {
        case debug_state::stopped:
            title = "Board paused";
            break;
        case debug_state::running:
            title = "Board running";
            break;
        case debug_state::lost:
            title = "Board not answering";
            break;
        case debug_state::none:
            break;
        }
        return make(ui_model::tone::neutral, "DEBUGGING", title, "An engineer is debugging this board.\nDo not unplug it.");
    }

    const char *link_text(link l)
    {
        switch (l) {
        case link::usb:
            return "USB connected";
        case link::network:
            return "Network connected"; // Wi-Fi or cellular
        case link::none:
            break;
        }
        return "";
    }
} // namespace ui_screens
