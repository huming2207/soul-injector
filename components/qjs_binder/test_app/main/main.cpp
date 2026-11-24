/*
 * qjs_binder against a mock target. The test task plays the programming task
 * (owner); test.js picks its scenario from the job name. After every run the
 * system heap must be back where it was.
 *
 * Last scenario: start a run and panic. The next boot must refuse that script.
 */

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "qjs_binder.hpp"

using namespace qjs_binder;

extern const uint8_t qbc[] asm("_binary_test_qbc_start");
extern const uint8_t qbc_end[] asm("_binary_test_qbc_end");
extern const uint8_t stm_qbc[] asm("_binary_stm32wl_qbc_start");
extern const uint8_t stm_qbc_end[] asm("_binary_stm32wl_qbc_end");

static int fails;

static void check(bool ok, const char *what, const char *detail = "")
{
    printf("BINDER %-44s %s %s\n", what, ok ? "ok" : "FAIL", ok ? "" : detail);
    fails += !ok;
}

class mock_target final : public js_target
{
public:
    char log[256] = {};
    uint32_t mem = 0x77;
    int polls = 0;

    void add(const char *s)
    {
        strlcat(log, s, sizeof(log));
        strlcat(log, " ", sizeof(log));
    }

    esp_err_t read32(uint32_t addr, uint32_t *val) override
    {
        char s[16];
        snprintf(s, sizeof(s), "R%lx", (unsigned long)addr);
        add(s);
        switch (addr) {
        case 0x1000:
            *val = mem;
            return ESP_OK;
        case 0x2000:
            *val = ++polls >= 3;
            return ESP_OK;
        case 0xdead:
            vTaskDelay(pdMS_TO_TICKS(600));
            *val = 0xdead;
            return ESP_OK;
        default:
            *val = 0;
            return ESP_OK;
        }
    }

    esp_err_t write32(uint32_t addr, uint32_t val) override
    {
        char s[24];
        snprintf(s, sizeof(s), "W%lx=%lx", (unsigned long)addr, (unsigned long)val);
        add(s);
        if (addr == 0xbad) {
            return ESP_FAIL;
        }
        mem = val;
        return ESP_OK;
    }

    esp_err_t halt() override { add("H"); return ESP_OK; }
    esp_err_t wait_halt() override { add("WH"); return ESP_OK; }
    esp_err_t reset() override { add("RS"); return ESP_OK; }
    esp_err_t reinit() override { add("RI"); return ESP_OK; }
};

static int64_t cancel_at_us;
static char pre_err[96];

static bool cancel_soon()
{
    return cancel_at_us && esp_timer_get_time() >= cancel_at_us;
}

struct outcome {
    esp_err_t load, pre, post;
    int64_t pre_ms;
};

static outcome run(const char *name, mock_target &m, uint32_t timeout_ms = 0, const uint8_t *pin = nullptr)
{
    auto *h = js_host::instance();
    run_info info = {name, "variant", "mock"};
    outcome o = {ESP_FAIL, ESP_FAIL, ESP_FAIL, 0};
    o.load = h->begin_run(qbc, qbc_end - qbc, pin, info);
    if (o.load == ESP_OK) {
        int64_t t0 = esp_timer_get_time();
        o.pre = h->has_hook(hook::pre_program) ? h->run_hook(hook::pre_program, m, timeout_ms, cancel_soon) : ESP_ERR_NOT_FOUND;
        o.pre_ms = (esp_timer_get_time() - t0) / 1000;
        cancel_at_us = 0;
        char err[96];
        strlcpy(err, h->last_error(), sizeof(err));
        o.post = h->run_hook(hook::post_program, m, timeout_ms, cancel_soon);
        if (o.pre != ESP_OK) {
            strlcpy(pre_err, err, sizeof(pre_err)); // what the failed hook said
        }
    }
    h->end_run();
    vTaskDelay(pdMS_TO_TICKS(20));
    return o;
}

static void test_task(void *)
{
    auto *h = js_host::instance();
    check(h->init() == ESP_OK, "init");

    if (esp_reset_reason() == ESP_RST_PANIC) {
        mock_target m;
        outcome o = run("ops", m);
        check(o.load == ESP_ERR_INVALID_STATE && strstr(h->last_error(), "crashed"), "script that crashed is refused",
              h->last_error());
        printf(fails ? "BINDER DONE FAIL (%d)\n" : "BINDER DONE PASS\n", fails);
        vTaskDelete(nullptr);
    }

    { // warm-up: first use of the task, the arena and logging
        mock_target m;
        run("ops", m);
    }
    size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ext0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    auto heap_same = [&](const char *name) {
        long di = (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (long)int0;
        long de = (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (long)ext0;
        char d[48];
        snprintf(d, sizeof(d), "heap %+ld/%+ld", di, de);
        char what[64];
        snprintf(what, sizeof(what), "%s: heap unchanged", name);
        check(di == 0 && de == 0, what, d);
    };

    {
        mock_target m;
        outcome o = run("ops", m);
        check(o.load == ESP_OK && o.pre == ESP_OK && o.post == ESP_OK, "ops: hooks pass", h->last_error());
        const char *want = "W1000=5 R1000 W1000=aa R1000 R2000 R2000 R2000 H RS RI WH ";
        check(strcmp(m.log, want) == 0, "ops: target saw the right calls", m.log);
        heap_same("ops");
    }
    {
        mock_target m;
        outcome o = run("throw", m);
        check(o.pre == ESP_FAIL && strstr(pre_err, "boom"), "throw: hook fails with the message", pre_err);
        heap_same("throw");
    }
    {
        mock_target m;
        outcome o = run("spin", m);
        check(o.pre == ESP_ERR_TIMEOUT && o.pre_ms < 1000, "spin: stopped by the slice", pre_err);
        check(strstr(pre_err, "without yielding") != nullptr, "spin: says why", pre_err);
        check(o.post == ESP_ERR_INVALID_STATE, "spin: no more hooks after that");
        heap_same("spin");
    }
    {
        mock_target m;
        outcome o = run("forever", m, 300);
        char d[32];
        snprintf(d, sizeof(d), "%lld ms", o.pre_ms);
        check(o.pre == ESP_ERR_TIMEOUT && o.pre_ms >= 300 && o.pre_ms < 1000, "forever: hook timeout", d);
        heap_same("forever");
    }
    {
        mock_target m;
        outcome o = run("slow", m);
        check(o.pre == ESP_OK, "slow: call times out, late answer ignored", h->last_error());
        heap_same("slow");
    }
    {
        mock_target m;
        outcome o = run("pollfail", m);
        check(o.pre == ESP_OK, "pollfail: poll32 rejects with TIMEOUT", h->last_error());
    }
    {
        mock_target m;
        outcome o = run("busy", m);
        check(o.pre == ESP_OK, "busy: sync call during async is BUSY", h->last_error());
    }
    {
        mock_target m;
        outcome o = run("fault", m);
        check(o.pre == ESP_OK && strcmp(m.log, "Wbad=1 Wbad=1 Wbad=1 ") == 0, "fault: retried twice, then FAULT", m.log);
    }
    {
        mock_target m;
        cancel_at_us = esp_timer_get_time() + 100 * 1000;
        outcome o = run("cancel", m);
        char d[32];
        snprintf(d, sizeof(d), "%lld ms", o.pre_ms);
        check(o.pre != ESP_OK && o.pre_ms < 500, "cancel: hook ends soon after", d);
        heap_same("cancel");
    }
    {
        mock_target m;
        uint8_t sha[32];
        bool pinned = true;
        check(h->last_script(sha, &pinned) && !pinned, "hash of the last script, not pinned");
        sha[0] ^= 1;
        outcome o = run("ops", m, 0, sha);
        check(o.load == ESP_ERR_INVALID_CRC, "pin: other script refused");
        sha[0] ^= 1;
        o = run("ops", m, 0, sha);
        check(o.load == ESP_OK && o.pre == ESP_OK && h->last_script(sha, &pinned) && pinned, "pin: matching script runs");
    }
    {
        static const uint8_t junk[] = "QJBC\x01\x00\x00\x00\x04\x00\x00\x00junk";
        run_info info = {"junk", "", "mock"};
        esp_err_t ret = h->begin_run(junk, sizeof(junk) - 1, nullptr, info);
        h->end_run();
        check(ret != ESP_OK && h->last_error()[0], "junk bytecode refused", h->last_error());
        heap_same("junk");
    }

    { // scripts/stm32wl-rdp.js, step for step pre_prog.yaml / post_prog.yaml
        mock_target pre, post;
        run_info info = {"wl", "STM32WLE5CC", "cortex-m"};
        esp_err_t load = h->begin_run(stm_qbc, stm_qbc_end - stm_qbc, nullptr, info);
        esp_err_t r1 = load == ESP_OK ? h->run_hook(hook::pre_program, pre, 0, nullptr) : load;
        esp_err_t r2 = load == ESP_OK ? h->run_hook(hook::post_program, post, 0, nullptr) : load;
        h->end_run();
        const char *steps = "H W58004008=45670123 W58004008=cdef89ab R58004014 W5800400c=8192a3b W5800400c=4c5d6e7f "
                            "R58004014 R58004010 R58004020 W58004020=%s R58004014 W58004014=20000 R58004010 "
                            "R58004014 W58004014=8000000 RI ";
        char want[256];
        snprintf(want, sizeof(want), steps, "aa");
        check(r1 == ESP_OK && strcmp(pre.log, want) == 0, "stm32wl: preProgram = pre_prog.yaml", pre.log);
        snprintf(want, sizeof(want), steps, "bb");
        check(r2 == ESP_OK && strcmp(post.log, want) == 0, "stm32wl: postProgram = post_prog.yaml", post.log);
        vTaskDelay(pdMS_TO_TICKS(20));
        heap_same("stm32wl");
    }

    printf("BINDER phase 1: %d failed. Now panicking in the middle of a run.\n", fails);
    if (fails) {
        printf("BINDER DONE FAIL (%d)\n", fails);
        vTaskDelete(nullptr);
    }
    run_info info = {"ops", "", "mock"};
    h->begin_run(qbc, qbc_end - qbc, nullptr, info);
    abort();
}

extern "C" void app_main()
{
    // The owner, on the other core from the JS task like the programming task
    xTaskCreatePinnedToCore(test_task, "owner", 8192, nullptr, 10, nullptr, 1);
}
