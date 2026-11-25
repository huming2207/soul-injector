#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

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
extern const uint8_t sess_bin[] asm("_binary_session_bin_start");
extern const uint8_t sess_bin_end[] asm("_binary_session_bin_end");

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

    uint32_t mem_end = 0; // reads that cross it fail like a bus fault; 0: none do

    esp_err_t read_mem(uint32_t addr, uint8_t *buf, size_t len) override
    {
        char s[32];
        snprintf(s, sizeof(s), "RM%lx+%u", (unsigned long)addr, (unsigned)len);
        add(s);
        if (len > 4096) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (mem_end && addr + len > mem_end) {
            return ESP_FAIL;
        }
        for (size_t i = 0; i < len; i++) {
            buf[i] = ram[i % sizeof(ram)];
        }
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

    esp_err_t resume() override { add("RU"); halted = false; dfsr = 0; return ESP_OK; }

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

    bool lockup = false, reset_seen = false;
    uint32_t dfsr = 0, vc = 0;

    esp_err_t poll_core(core_state *st) override // not logged: it's called every few ms
    {
        *st = {halted, lockup, reset_seen, dfsr};
        reset_seen = false;
        return ESP_OK;
    }

    esp_err_t set_vector_catch(uint32_t v) override { vc = v; return ESP_OK; }
    esp_err_t clear_stop_cause() override { dfsr = 0; return ESP_OK; }
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


static std::atomic<int> notified;
static void on_notify(uint32_t) { notified++; }

static const uint8_t *piece(const char *name, size_t *len)
{
    for (const uint8_t *p = sess_bin; p < sess_bin_end;) {
        size_t n = strlen((const char *)p);
        const uint8_t *q = p + n + 1;
        uint32_t l;
        memcpy(&l, q, 4);
        if (strcmp((const char *)p, name) == 0) {
            *len = l;
            return q + 4;
        }
        p = q + 4 + l;
    }
    printf("BINDER no session piece %s\n", name);
    abort();
}

struct sres {
    esp_err_t ret;
    script_outcome outcome;
    bool truncated;
    std::string value, error, stack;
    int64_t ms;
};

static sres session_call(bool run, const char *name, mock_target &m, uint32_t timeout_ms = 0)
{
    auto *h = js_host::instance();
    size_t len;
    const uint8_t *p = piece(name, &len);
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = run ? h->session_run(p, len, timeout_ms, m) : h->session_eval(p, len, timeout_ms, m);
    script_result r = h->last_script_result();
    return {ret, r.outcome, r.truncated, r.value, r.error, r.stack, (esp_timer_get_time() - t0) / 1000};
}

static sres eval(const char *name, mock_target &m, uint32_t timeout_ms = 0) { return session_call(false, name, m, timeout_ms); }
static sres run_mod(const char *name, mock_target &m) { return session_call(true, name, m); }

static void serve_for(mock_target &m, int ms)
{
    int64_t end = esp_timer_get_time() + ms * 1000LL;
    while (esp_timer_get_time() < end) {
        js_host::instance()->session_service(m, 5);
    }
}

static uint32_t ev_seen;
static event_out evs[8];

// the first event after the last one looked at, of this kind, containing this text; 0 if none
static uint32_t find_event(const char *kind, const char *text, uint32_t *capture = nullptr)
{
    auto *h = js_host::instance();
    uint32_t newest = 0, found = 0;
    for (;;) {
        size_t n = h->read_events(ev_seen, evs, 8, &newest, nullptr);
        if (n == 0) {
            break;
        }
        for (size_t i = 0; i < n; i++) {
            ev_seen = evs[i].seq;
            if (!found && strcmp(evs[i].kind, kind) == 0 && strstr(evs[i].json, text)) {
                found = evs[i].seq;
                if (capture) {
                    *capture = evs[i].capture_id;
                }
            }
        }
    }
    return found;
}

static void session_tests()
{
    auto *h = js_host::instance();
    size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ext0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mock_target m;
    m.regs[15] = 0x08000123;
    session_config cfg = {{false, false, false}, true, on_notify, nullptr};
    check(h->begin_session(cfg) == ESP_OK && h->in_session(), "session: begins");
    check(h->begin_session(cfg) == ESP_ERR_INVALID_STATE, "session: only one at a time");
    run_info info = {"ops", "", "mock", false};
    check(h->begin_run(qbc, qbc_end - qbc, nullptr, info) == ESP_ERR_INVALID_STATE && strstr(h->last_error(), "session"),
          "session: job runs wait for it", h->last_error());

    sres r = eval("add", m);
    check(r.ret == ESP_OK && r.outcome == script_outcome::ok && r.value == "3", "session: eval 1 + 2", r.error.c_str());
    eval("setx", m);
    r = eval("usex", m);
    check(r.value == "10", "session: evals share globals", r.value.c_str());
    r = eval("read", m);
    check(r.value == "119" && strstr(m.log, "R1000"), "session: eval reads the target", r.value.c_str());
    r = eval("throw", m);
    check(r.outcome == script_outcome::error && r.error.find("nope") != std::string::npos && !r.stack.empty(),
          "session: a throw gives error and stack", r.error.c_str());
    r = eval("write", m);
    check(r.outcome == script_outcome::error && r.error.find("--allow-write") != std::string::npos && !strstr(m.log, "W1000"),
          "session: writes need --allow-write", r.error.c_str());
    r = eval("types", m);
    check(r.value == "[[1,2],\"10\",null,\"Error: e\"]", "session: typed arrays, BigInt, Error as JSON", r.value.c_str());
    r = eval("undef", m);
    check(r.value == "undefined", "session: undefined", r.value.c_str());
    int before = notified;
    r = eval("console", m);
    check(r.value == "\"logged\"" && find_event("console", "hi there") && notified > before, "session: console is an event");
    r = eval("emit", m);
    check(r.value == "true" && find_event("emit", "{\"name\":\"ping\",\"data\":{\"a\":1}}"), "session: agent.emit", r.value.c_str());
    r = eval("slow", m, 50);
    char d0[160];
    snprintf(d0, sizeof(d0), "%d %s %s", (int)r.outcome, r.value.c_str(), r.error.c_str());
    check(r.outcome == script_outcome::pending, "session: slow eval is pending at its timeout", d0);
    serve_for(m, 400);
    check(find_event("result", "\"outcome\":\"ok\",\"text\":\"7\"") != 0, "session: its result comes as an event");
    r = eval("big", m);
    check(r.truncated && r.value.size() == RESULT_VALUE_MAX, "session: big value truncated");
    r = eval("reject", m);
    check(r.outcome == script_outcome::error && r.error.find("bad") != std::string::npos, "session: rejection is an error",
          r.error.c_str());
    r = eval("uncaught", m);
    serve_for(m, 50);
    snprintf(d0, sizeof(d0), "%d %s %s", (int)r.outcome, r.value.c_str(), r.error.c_str());
    check(r.value == "1" && find_event("error", "late boom"), "session: uncaught errors are events", d0);
    r = eval("perms", m);
    check(r.value == "{\"write\":false,\"flash\":false,\"reset\":false}", "session: agent.permissions()", r.value.c_str());
    r = eval("waithalt", m);
    check(r.outcome == script_outcome::error && r.error.find("TIMEOUT") != std::string::npos && r.ms >= 90 && r.ms < 400,
          "session: waitHalt({timeoutMs})", r.error.c_str());

    // A session steps delay/poll32/waitHalt between other work, so a sync call
    // needn't wait for them: watch handlers read memory while the module awaits.
    r = eval("syncdelay", m);
    check(r.value == "0", "session: sync call while a delay is pending", r.error.c_str());

    r = run_mod("watchers", m);
    serve_for(m, 20);
    check(r.outcome == script_outcome::ok && m.vc == 0x7F0, "session: run, fault watch sets vector catch", r.error.c_str());
    r = eval("watches", m);
    check(r.value == "\"fault:halt,halt:halt\"", "session: agent.watches()", r.value.c_str());
    r = eval("typeofx", m);
    check(r.value == "\"undefined\"", "session: run replaced the runtime", r.value.c_str());

    m.halted = true;
    m.dfsr = 2;
    uint32_t cap = 0, mark = ev_seen;
    serve_for(m, 200);
    uint32_t w = find_event("watch", "\"action\":\"resume\",\"handler\":\"ok\"", &cap);
    ev_seen = mark;
    bool emitted = find_event("emit", "\"cause\":\"breakpoint\",\"pc\":134218019,\"regPc\":134218019,\"mem\":1,\"stack\":256");
    check(w && !m.halted && strstr(m.log, "RU"), "session: halt watch, handler resumes", m.log);
    check(emitted, "session: handler gets regs, stack and mem");
    uint8_t buf[64];
    size_t got = 0;
    uint32_t total = 0;
    check(cap && h->read_capture(cap, 0, buf, sizeof(buf), &got, &total) == ESP_OK && memcmp(buf, "SICP", 4) == 0 && total > 256,
          "session: capture readable");

    // Stacks start at the top of RAM, so the default capture runs past its end;
    // it used to lose the whole chunk that crossed it.
    uint32_t sp = m.regs[13];
    m.regs[13] = 0x2000ffc0; // thread mode: the capture starts at sp, 64 bytes below the end
    m.mem_end = 0x20010000;
    m.halted = true;
    m.dfsr = 2;
    mark = ev_seen;
    serve_for(m, 200);
    ev_seen = mark;
    check(find_event("emit", "\"mem\":1,\"stack\":64}") && !m.halted, "session: stack capture keeps what's readable",
          m.log);
    m.mem_end = 0;
    m.regs[13] = sp;

    m.halted = true;
    m.dfsr = 8;
    m.regs[16] = 3;
    serve_for(m, 200);
    check(find_event("watch", "\"cause\":\"HardFault\",\"reflex\":\"halt\",\"action\":\"hold\"") && m.halted,
          "session: fault watch holds the target");
    r = eval("seen", m);
    check(r.value == "\"breakpoint\"", "session: module and eval share globalThis", r.value.c_str());
    r = eval("watchdup", m);
    check(r.outcome == script_outcome::error && r.error.find("BUSY") != std::string::npos, "session: one watch per kind",
          r.error.c_str());
    r = eval("resetreflex", m);
    check(r.outcome == script_outcome::error, "session: a reset watch can't reset", r.error.c_str());
    m.halted = false;
    serve_for(m, 20);

    r = run_mod("empty", m);
    serve_for(m, 20);
    check(r.outcome == script_outcome::ok && m.vc == 0, "session: new run drops the old watches");
    m.halted = true;
    m.dfsr = 2;
    serve_for(m, 50);
    check(find_event("stop", "breakpoint") && m.halted, "session: unwatched halt is a stop event");
    m.halted = false;
    serve_for(m, 20);

    r = run_mod("stuck", m);
    m.halted = true;
    m.dfsr = 2;
    serve_for(m, 300);
    check(find_event("watch", "\"action\":\"hold\",\"handler\":\"timeout\"") && m.halted, "session: handler timeout holds");
    m.halted = false;
    serve_for(m, 20);

    r = run_mod("bad", m);
    check(r.outcome == script_outcome::error && r.error.find("nope") != std::string::npos, "session: run that fails to link",
          r.error.c_str());

    // session_start/session_poll_result: what a trap's owner uses so it can go
    // on serving others while a module starts.
    {
        size_t len;
        const uint8_t *p = piece("bad", &len);
        uint32_t token = 0;
        bool ok = h->session_start(p, len, &token) == ESP_OK && token != 0;
        int64_t end = esp_timer_get_time() + 2000000;
        bool done = false;
        while (ok && !(done = h->session_poll_result(token, false, m)) && esp_timer_get_time() < end) {
            h->session_service(m, 5);
        }
        script_result sr = h->last_script_result();
        check(done && sr.outcome == script_outcome::error && strstr(sr.error, "nope"), "session: start, then poll its failure", sr.error);

        p = piece("forever", &len);
        ok = h->session_start(p, len, &token) == ESP_OK && token != 0;
        end = esp_timer_get_time() + 300000;
        done = false;
        while (ok && !done && esp_timer_get_time() < end) {
            done = h->session_poll_result(token, false, m);
            h->session_service(m, 5);
        }
        bool given_up = h->session_poll_result(token, true, m);
        sr = h->last_script_result();
        check(ok && !done && given_up && sr.outcome == script_outcome::pending && h->session_running(),
              "session: start, a top level that goes on is pending", sr.error);
        check(!h->module_failed(), "session: a module still running hasn't failed");

        // A top level that fails after it started leaves the runtime running; a trap
        // must still see it (module_failed) to restart the module.
        p = piece("latefail", &len);
        ok = h->session_start(p, len, &token) == ESP_OK && token != 0;
        end = esp_timer_get_time() + 2000000;
        while (ok && !h->session_poll_result(token, esp_timer_get_time() >= end, m)) {
            h->session_service(m, 5);
        }
        serve_for(m, 50);
        check(ok && h->module_failed() && h->session_running(), "session: a module that fails late is seen");
    }
    r = eval("spin", m);
    check(r.outcome == script_outcome::stopped && r.error.find("yield") != std::string::npos, "session: spin is stopped",
          r.error.c_str());
    check(find_event("runtime", "yield") != 0, "session: runtime end is an event");
    r = eval("add", m);
    check(r.value == "3", "session: next eval starts a runtime", r.error.c_str());
    for (const char *name : {"oom", "oomlate"}) {
        int64_t t0 = esp_timer_get_time();
        r = eval(name, m, 5000);
        char d[160];
        snprintf(d, sizeof(d), "%s: %lld ms, %s", name, (esp_timer_get_time() - t0) / 1000, r.error.c_str());
        check(r.outcome == script_outcome::stopped && r.error.find("out of memory") != std::string::npos &&
                  esp_timer_get_time() - t0 < 3000000,
              "session: out of memory ends the runtime at once", d);
        r = eval("add", m);
        check(r.value == "3", "session: eval after out of memory", r.error.c_str());
    }
    h->session_stop(m);
    r = eval("typeofx", m);
    check(r.value == "\"undefined\"", "session: stop, then eval again", r.value.c_str());
    h->end_session(m);
    check(!h->in_session() && m.vc == 0, "session: ends, vector catch off");

    cfg.perms = {true, true, true};
    h->begin_session(cfg);
    m.log[0] = '\0';
    r = eval("write", m);
    snprintf(d0, sizeof(d0), "%d %s %s", (int)r.outcome, r.value.c_str(), r.error.c_str());
    check(r.outcome == script_outcome::ok && strstr(m.log, "W1000=1"), "session: --allow-write lets it write", d0);
    h->end_session(m);

    mock_target j;
    outcome o = run("ops", j);
    check(o.load == ESP_OK && o.pre == ESP_OK && o.post == ESP_OK, "session: job runs work again after it", h->last_error());

    vTaskDelay(pdMS_TO_TICKS(20));
    int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ext0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    { // the same paths again, warm
        mock_target k;
        h->begin_session(cfg);
        eval("add", k);
        eval("throw", k);
        eval("slow", k, 50);
        run_mod("watchers", k);
        k.halted = true;
        k.dfsr = 2;
        serve_for(k, 200);
        k.halted = true;
        k.dfsr = 8;
        k.regs[16] = 3;
        serve_for(k, 200);
        eval("spin", k);
        eval("add", k);
        h->end_session(k);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    long di = (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (long)int0;
    long de = (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (long)ext0;
    char d[48];
    snprintf(d, sizeof(d), "heap %+ld/%+ld", di, de);
    check(di == 0 && de == 0, "session: heap unchanged", d);
}

static std::string inc_log;
static uint32_t inc_next = 40;
static bool inc_capture;

static uint32_t mock_open(const incident_open &o)
{
    inc_log += o.watch ? "W:" : "S:";
    inc_log += o.name;
    inc_log += o.cause_json;
    inc_capture = o.capture && o.capture_len > 16 && memcmp(o.capture, "SICP", 4) == 0;
    return ++inc_next;
}

static void mock_add(uint32_t id, incident_part part, const char *json, size_t len)
{
    inc_log += part == incident_part::data ? " D" : " O";
    inc_log += std::to_string(id) + ":";
    inc_log.append(json, len);
    inc_log += " ";
}

static void trap_tests()
{
    auto *h = js_host::instance();
    mock_target m;
    session_config cfg = {{false, false, false}, true, on_notify, nullptr};
    h->begin_session(cfg);
    sres r = eval("incident", m);
    check(r.value == "0" && find_event("incident", "\"name\":\"bad-crc\",\"incident\":0,\"data\":{\"n\":3}"),
          "trap: no incidents in a plain session, still an event", r.value.c_str());
    h->end_session(m);

    static const incident_hooks hooks = {mock_open, mock_add};
    cfg.incidents = &hooks;
    h->begin_session(cfg);
    r = eval("incident", m);
    check(r.value == "41" && inc_log == "S:bad-crc{\"kind\":\"script\",\"name\":\"bad-crc\"} D41:{\"n\":3} ",
          "trap: agent.incident records one", inc_log.c_str());
    r = eval("incidentbig", m);
    check(r.outcome == script_outcome::error && r.error.find("8191") != std::string::npos, "trap: incident data has a limit",
          r.error.c_str());
    r = eval("incidentname", m);
    check(r.outcome == script_outcome::error, "trap: incident needs a name", r.error.c_str());
    r = eval("incident", m);
    check(r.value == "42", "trap: the buffer is free again after an error", r.error.c_str());

    inc_log.clear();
    r = run_mod("trap", m);
    serve_for(m, 20);
    m.halted = true;
    m.dfsr = 2;
    serve_for(m, 200);
    bool watch_ev = find_event("watch", "\"pc\":\"0x00000000\",\"incident\":43");
    check(watch_ev && !m.halted, "trap: watch event names its incident", inc_log.c_str());
    check(inc_capture && inc_log.find("W:bp{\"kind\":\"watch\",\"watch\":") == 0 && inc_log.find("\"first_action\":\"hold\"") != std::string::npos,
          "trap: a firing opens an incident with its capture", inc_log.c_str());
    check(inc_log.find(" D43:{\"hist\":[1,2,3]} ") != std::string::npos && inc_log.find(" O43:{\"action\":\"resume\"") != std::string::npos,
          "trap: handler data and outcome are added", inc_log.c_str());
    ev_seen = 0;
    check(find_event("emit", "{\"name\":\"inc\",\"data\":{\"id\":43}}"), "trap: the handler sees incidentId");

    inc_log.clear();
    m.halted = true;
    m.dfsr = 8;
    m.regs[16] = 3;
    serve_for(m, 100);
    check(inc_log.find("\"on\":\"fault\"") != std::string::npos && inc_log.find("\"first_action\":\"resume\"") != std::string::npos &&
              inc_log.find(" O44:{\"action\":\"resume\",\"handler\":\"none\"") != std::string::npos && !m.halted,
          "trap: reflex none records and lets it run", inc_log.c_str());
    h->end_session(m);
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
    // A runtime that dies between the hooks (here in onProgress) must fail the
    // next hook: postProgram used to be skipped and the run passed.
    {
        mock_target m;
        void (*between)() = [] {
            js_host::instance()->progress("program", 50);
            vTaskDelay(pdMS_TO_TICKS(500));
        };
        outcome o = run("progdie", m, 2000, nullptr, false, between);
        const char *err = h->last_error();
        check(o.pre == ESP_OK && o.post == ESP_ERR_INVALID_STATE && strstr(err, "between hooks") && strstr(err, "yielding"),
              "a runtime that dies between hooks fails the next one", err);
        heap_same("progdie");
    }
    // QuickJS-NG's "out of memory" is uncatchable; it used to leave the hook
    // waiting for its timeout instead of failing it.
    for (const char *name : {"oom", "oomlate"}) {
        mock_target m;
        outcome o = run(name, m, 5000);
        char d[160];
        snprintf(d, sizeof(d), "%s: %lld ms, %s", name, o.pre_ms, pre_err);
        check(o.pre == ESP_FAIL && o.pre_ms < 3000, "out of memory: the hook fails at once, not at its timeout", d);
        check(strstr(pre_err, "out of memory") != nullptr, "out of memory: says why", d);
        check(o.post == ESP_ERR_INVALID_STATE, "out of memory: no more hooks after that", d);
        heap_same(name);
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

    session_tests();
    trap_tests();

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
