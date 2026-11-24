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

    uint8_t ram[64] = {};
    uint8_t flash[16] = {};

    esp_err_t read_mem(uint32_t addr, uint8_t *buf, size_t len) override
    {
        char s[32];
        snprintf(s, sizeof(s), "RM%lx+%u", (unsigned long)addr, (unsigned)len);
        add(s);
        if (len > sizeof(ram)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(buf, ram, len);
        return ESP_OK;
    }

    esp_err_t write_mem(uint32_t addr, const uint8_t *buf, size_t len) override
    {
        char s[32];
        snprintf(s, sizeof(s), "WM%lx+%u", (unsigned long)addr, (unsigned)len);
        add(s);
        if (len > sizeof(ram)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(ram, buf, len);
        return ESP_OK;
    }

    esp_err_t flash_erase(uint32_t addr, size_t len) override
    {
        char s[32];
        snprintf(s, sizeof(s), "FE%lx+%x", (unsigned long)addr, (unsigned)len);
        add(s);
        memset(flash, 0xff, sizeof(flash));
        return ESP_OK;
    }

    esp_err_t flash_write(uint32_t addr, const uint8_t *buf, size_t len) override
    {
        char s[32];
        snprintf(s, sizeof(s), "FW%lx+%u", (unsigned long)addr, (unsigned)len);
        add(s);
        memcpy(flash, buf, len < sizeof(flash) ? len : sizeof(flash));
        return ESP_OK;
    }

    esp_err_t flash_read(uint32_t addr, uint8_t *buf, size_t len) override
    {
        char s[32];
        snprintf(s, sizeof(s), "FR%lx+%u", (unsigned long)addr, (unsigned)len);
        add(s);
        vTaskDelay(pdMS_TO_TICKS(20));
        memcpy(buf, flash, len < sizeof(flash) ? len : sizeof(flash));
        return ESP_OK;
    }

    esp_err_t call(uint32_t addr, const uint32_t args[4], uint32_t stack_top, uint32_t timeout_ms, cancel_fn,
                   uint32_t *r0) override
    {
        char s[48];
        snprintf(s, sizeof(s), "C%lx/%lx", (unsigned long)addr, (unsigned long)stack_top);
        add(s);
        if (addr == 0xdead) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        *r0 = args[0] + args[1] + args[2] + args[3];
        return ESP_OK;
    }

    bool halted = false;
    uint32_t regs[128] = {};

    esp_err_t resume() override { add("RU"); halted = false; return ESP_OK; }

    esp_err_t read_reg(uint32_t id, uint32_t *val) override
    {
        char s[16];
        snprintf(s, sizeof(s), "RG%lx", (unsigned long)id);
        add(s);
        *val = regs[id & 0x7f];
        return halted ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    esp_err_t write_reg(uint32_t id, uint32_t val) override
    {
        char s[24];
        snprintf(s, sizeof(s), "WG%lx=%lx", (unsigned long)id, (unsigned long)val);
        add(s);
        regs[id & 0x7f] = val;
        return halted ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    esp_err_t halt() override { add("H"); halted = true; return ESP_OK; }
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

static outcome run(const char *name, mock_target &m, uint32_t timeout_ms = 0, const uint8_t *pin = nullptr,
                   bool network = false, void (*between)() = nullptr)
{
    auto *h = js_host::instance();
    run_info info = {name, "variant", "mock", network};
    outcome o = {ESP_FAIL, ESP_FAIL, ESP_FAIL, 0};
    o.load = h->begin_run(qbc, qbc_end - qbc, pin, info);
    if (o.load == ESP_OK) {
        int64_t t0 = esp_timer_get_time();
        o.pre = h->has_hook(hook::pre_program) ? h->run_hook(hook::pre_program, m, timeout_ms, cancel_soon) : ESP_ERR_NOT_FOUND;
        o.pre_ms = (esp_timer_get_time() - t0) / 1000;
        cancel_at_us = 0;
        char err[96];
        strlcpy(err, h->last_error(), sizeof(err));
        if (between) {
            between();
        }
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

    { // warm-up
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
        run_info info = {"junk", "", "mock", false};
        esp_err_t ret = h->begin_run(junk, sizeof(junk) - 1, nullptr, info);
        h->end_run();
        check(ret != ESP_OK && h->last_error()[0], "junk bytecode refused", h->last_error());
        heap_same("junk");
    }

    {
        mock_target m;
        outcome o = run("mem", m);
        check(o.pre == ESP_OK && strcmp(m.log, "WM20000000+8 RM20000000+8 ") == 0, "mem: writeMem/readMem", pre_err[0] ? pre_err : m.log);
    }
    {
        mock_target m;
        outcome o = run("flash", m);
        const char *want = "FE803f800+800 FW803f800+4 FR803f800+4 FR803f800+2 FR803f800+4 ";
        check(o.pre == ESP_OK && strcmp(m.log, want) == 0, "flash: erase/write/read, one buffer at a time", pre_err[0] ? pre_err : m.log);
        heap_same("flash");
    }
    {
        mock_target m;
        outcome o = run("call", m);
        check(o.pre == ESP_OK && strcmp(m.log, "C20001000/20008000 Cdead/20008000 ") == 0, "call: args, r0, UNSUPPORTED",
              pre_err[0] ? pre_err : m.log);
    }
    {
        mock_target m;
        outcome o = run("regs", m);
        const char *want = "H WG0=5 WG43=3f800000 WG14=7 RG0 RG43 RG14 RU RGf ";
        check(o.pre == ESP_OK && strcmp(m.log, want) == 0, "regs: names, numbers, NOT_HALTED, resume", pre_err[0] ? pre_err : m.log);
    }
    {
        mock_target m;
        run_info info = {"fail", "", "mock", false};
        esp_err_t load = h->begin_run(qbc, qbc_end - qbc, nullptr, info);
        bool hooks = load == ESP_OK && h->has_hook(hook::test) && h->has_hook(hook::on_fail);
        esp_err_t tr = h->run_hook(hook::test, m, 0, nullptr);
        esp_err_t pre = h->run_hook(hook::pre_program, m, 0, nullptr);
        esp_err_t f = h->run_on_fail("preProgram", nullptr, m, 0, nullptr);
        char err[96];
        strlcpy(err, h->last_error(), sizeof(err));
        h->end_run();
        const char *res = h->last_results();
        check(hooks && tr == ESP_OK && strstr(res, "\"tested\":1"), "test: hook runs with its ctx", res);
        check(pre == ESP_FAIL && f == ESP_OK && strstr(res, "\"stage\":\"preProgram\"") && strstr(res, "boom2") &&
                  strcmp(m.log, "W1000=f ") == 0,
              "onFail: gets the stage and the error", res);
        check(strstr(err, "boom2") != nullptr, "onFail: last_error stays the first failure", err);
        heap_same("fail");
    }
    {
        mock_target m;
        run_info info = {"failfail", "", "mock", false};
        h->begin_run(qbc, qbc_end - qbc, nullptr, info);
        h->run_hook(hook::pre_program, m, 0, nullptr);
        esp_err_t f = h->run_on_fail("erase", "erase failed", m, 0, nullptr);
        char err[96];
        strlcpy(err, h->last_error(), sizeof(err));
        h->end_run();
        check(f == ESP_FAIL && strstr(h->last_results(), "erase failed") && strstr(err, "first"),
              "onFail: its own throw doesn't hide the first", err);
    }
    {
        mock_target m;
        run_info info = {"spin", "", "mock", false};
        h->begin_run(qbc, qbc_end - qbc, nullptr, info);
        esp_err_t pre = h->run_hook(hook::pre_program, m, 0, nullptr);
        bool gone = !h->has_hook(hook::on_fail) && h->run_on_fail("preProgram", nullptr, m, 0, nullptr) == ESP_ERR_NOT_FOUND;
        h->end_run();
        check(pre == ESP_ERR_TIMEOUT && gone, "onFail: not offered once the runtime stopped");
        heap_same("spin onFail");
    }
    {
        mock_target m;
        outcome o = run("crypto", m);
        check(o.pre == ESP_OK, "crypto: sha256 and randomBytes", pre_err);
    }
    {
        mock_target m;
        outcome o = run("log", m);
        const char *res = h->last_results();
        bool ok = o.pre == ESP_OK && o.post == ESP_FAIL && strstr(res, "\"serial\":\"SN0001\"") &&
                  strstr(res, "\"vdd\":{\"value\":3.31,\"lo\":3,\"hi\":3.6,\"unit\":\"V\",\"pass\":true}") &&
                  strstr(res, "\"idle_uA\":{\"value\":5,\"hi\":1,\"unit\":\"uA\",\"pass\":false}") && !strstr(res, "blob");
        check(ok, "log: record, check, too-big refused, failed check fails the hook", res);
        check(strstr(h->last_error(), "idle_uA = 5uA is out of limits") != nullptr, "log: failed check says why", h->last_error());
        heap_same("log");
    }
    {
        mock_target m;
        outcome o = run("progress", m, 0, nullptr, false, [] {
            auto *h = js_host::instance();
            for (int i = 0; i <= 100; i += 2) {
                h->progress("program", i);
                vTaskDelay(pdMS_TO_TICKS(5)); // ~250 ms of programming
            }
        });
        check(o.pre == ESP_OK && o.post == ESP_OK, "progress: merged, latest stage and 100% arrive", h->last_error());
    }
    {
        mock_target m;
        outcome off = run("net-off", m);
        outcome on = run("net-on", m, 0, nullptr, true);
        check(off.post == ESP_OK && on.post == ESP_OK, "network: esp:ws only when the job allows it", h->last_error());
    }

    { // stm32wl-rdp.js vs the YAML
        mock_target pre, post;
        run_info info = {"wl", "STM32WLE5CC", "cortex-m", false};
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
    run_info info = {"ops", "", "mock", false};
    h->begin_run(qbc, qbc_end - qbc, nullptr, info);
    abort();
}

extern "C" void app_main()
{
    xTaskCreatePinnedToCore(test_task, "owner", 8192, nullptr, 10, nullptr, 1);
}
