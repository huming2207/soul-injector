// Draws every screen into an RGB565 buffer and writes it as a PPM file.
// Usage: ui_preview <output dir>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <lvgl.h>
#include "ui_model.hpp"
#include "ui_view.hpp"

namespace
{
    constexpr int32_t W = ui_view::WIDTH;
    constexpr int32_t H = ui_view::HEIGHT;

    std::vector<uint16_t> frame(W *H);
    uint16_t draw_buf[W * H];

    void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
    {
        const auto *src = reinterpret_cast<const uint16_t *>(px_map);
        for (int32_t y = area->y1; y <= area->y2; y++) {
            for (int32_t x = area->x1; x <= area->x2; x++) {
                frame[y * W + x] = *src++;
            }
        }
        lv_display_flush_ready(disp);
    }

    bool write_ppm(const std::string &path)
    {
        FILE *f = fopen(path.c_str(), "wb");
        if (f == nullptr) {
            return false;
        }
        fprintf(f, "P6\n%d %d\n255\n", static_cast<int>(W), static_cast<int>(H));
        for (uint16_t px : frame) {
            // Expand RGB565 the way the panel shows it: the low bits are lost.
            uint8_t rgb[3] = {static_cast<uint8_t>(((px >> 11) & 0x1f) << 3), static_cast<uint8_t>(((px >> 5) & 0x3f) << 2),
                              static_cast<uint8_t>((px & 0x1f) << 3)};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        return true;
    }

    struct shot {
        const char *name;
        ui_model model;
        ui_screens::link link;
    };
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output dir>\n", argv[0]);
        return 2;
    }
    const std::string out = argv[1];

    lv_init();
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush);

    ui_view view;
    view.create(lv_screen_active());

    using ui_screens::link;
    const shot shots[] = {
        {"00-booting", ui_screens::booting(), link::none},
        {"01-ready", ui_screens::ready(false), link::none},
        {"02-ready-usb", ui_screens::ready(false), link::usb},
        {"03-ready-watching", ui_screens::ready(true), link::network},
        {"04-starting", ui_screens::starting(), link::none},
        {"05-connecting", ui_screens::connecting(), link::none},
        {"06-erasing-chip", ui_screens::erasing(-1), link::none},
        {"07-writing", ui_screens::writing(62), link::none},
        {"08-checking", ui_screens::checking(), link::none},
        {"09-testing", ui_screens::testing(1, 4, nullptr), link::none},
        {"10-testing-script", ui_screens::testing(0, 0, "Running the job's test script"), link::none},
        {"11-pass", ui_screens::passed(), link::none},
        {"12-fail-write", ui_screens::failed("Write failed", "Could not write the firmware.\nTry again. If it fails again, set the board aside."),
         link::none},
        {"13-fail-no-board", ui_screens::failed("No board found", "Check the cable and that the board has power."), link::none},
        {"14-fail-script", ui_screens::failed("Script error", "TypeError: cannot read property 'addr' of undefined at onTest (job.js:42)"),
         link::usb},
        {"15-notice-no-job", ui_screens::notice("No job", "Send a job from the computer."), link::none},
        {"16-notice-log-full", ui_screens::notice("Log is full", "Connect to the computer to collect the log."), link::none},
        {"17-notice-cancelled", ui_screens::notice("Stopped", "The job was cancelled."), link::usb},
        {"18-debug-running", ui_screens::debugging(ui_screens::debug_state::running), link::usb},
        {"19-debug-paused", ui_screens::debugging(ui_screens::debug_state::stopped), link::usb},
        {"20-debug-lost", ui_screens::debugging(ui_screens::debug_state::lost), link::network},
        {"21-setup", ui_screens::setup(), link::none},
        {"22-current", ui_screens::current(4.2, 1510.0, 612.5, "OK"), link::none},
    };

    int failures = 0;
    for (const shot &s : shots) {
        view.show(s.model);
        view.set_link(ui_screens::link_text(s.link));
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
        if (!write_ppm(out + "/" + s.name + ".ppm")) {
            fprintf(stderr, "can't write %s/%s.ppm\n", out.c_str(), s.name);
            failures += 1;
        }
    }
    return failures == 0 ? 0 : 1;
}
