#pragma once

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "qjs_binder.hpp"
#include "qjs_esp.h"
#include "quickjs.h"

namespace qjs_binder::priv
{
    enum op : uint8_t {
        OP_READ32,
        OP_WRITE32,
        OP_RMW32,
        OP_HALT,
        OP_READ_MEM,    // xfer
        OP_WRITE_MEM,   // xfer
        OP_WAIT_HALT,
        OP_RESET,
        OP_REINIT,
        OP_POLL32,
        OP_DELAY,
        OP_CALL,
        OP_FLASH_READ,  // xfer
        OP_FLASH_WRITE, // xfer
        OP_FLASH_ERASE,
        OP_RESUME,
        OP_READ_REG,
        OP_WRITE_REG,
        OP_WATCH_ACTION, // a watch handler's verdict: addr watch, a verdict, b fire, timeout_ms handler status
        OP_DONE,         // load/hook settled; in a session seq is the run/eval token
        OP_INCIDENT,
        OP_INCIDENT_DATA,
    };

    static constexpr uint8_t SYNC = 0xff;
    static constexpr int MAX_ASYNC = 4;
    static constexpr int HOOKS = 4;
    static constexpr int WATCHES = 4;     // one per watch_on
    static constexpr int WATCH_MEM = 4;   // capture.mem ranges
    static constexpr int CAPTURES = 4;
    static constexpr size_t CAPTURE_SIZE = 16384;
    static constexpr size_t CAPTURE_DATA_MAX = 12288; // stack + mem per watch
    static constexpr int EVENTS = 64;

    enum watch_on : uint8_t { ON_FAULT, ON_HALT, ON_LOCKUP, ON_RESET };
    enum reflex_t : uint8_t { REFLEX_HALT, REFLEX_NONE, REFLEX_RESET };
    enum verdict_t : uint8_t { V_HOLD, V_RESUME, V_RESET };
    enum handler_status : uint8_t { H_OK, H_THREW, H_BAD_VALUE, H_NONE };

    struct watch_def {
        bool used;
        bool regs;
        bool fault;
        bool has_handler;
        uint8_t reflex;
        uint8_t n_mem;
        uint16_t id;
        uint16_t stack;
        struct {
            uint32_t addr, len;
        } mem[WATCH_MEM];
        uint32_t handler_timeout_ms;
        char name[24];
    };

    struct capture_slot {
        uint32_t id;               // 0: empty
        uint32_t len;
        std::atomic<bool> js_busy; // the JS task is reading it
        uint8_t *data;             // CAPTURE_SIZE, PSRAM
    };

    struct fire_msg {
        uint32_t fire;
        uint32_t capture;
        uint32_t pc;
        uint16_t id;
        uint8_t on;
        uint8_t reflex;
        int8_t slot;               // capture slot, -1 for none
        bool held;                 // the owner waits for the verdict
        uint32_t incident;
        bool halted;
        char cause[16];
        char name[24];
    };

    struct engine {                // owner task only
        uint32_t gen;              // watch_gen applied
        uint32_t vc;               // vector catch bits set
        bool vc_dirty;
        bool known;                // halted/lockup below are current
        bool after_op;             // known was cleared by our own op: a stop found next is ours
        bool halted;
        bool lockup;
        bool ignore_reset;         // the next S_RESET_ST is our own
        bool reset_latch;          // a waitHalt poll saw S_RESET_ST
        bool lost;
        uint8_t fails;
        int64_t next_poll_us;
        bool waiting;              // a held target waits for a verdict
        bool wait_orphaned;        // its runtime was stopped: no verdict will come (a new one may be alive already)
        bool throttle_retrying;    // a throttle hold's resume failed and is being retried
        fire_msg w;
        int64_t w_until_us;
        uint32_t fires;
        int64_t window_us[WATCHES];
        uint16_t window_n[WATCHES];
        uint32_t captures;         // ids handed out
        int next_slot;
        int64_t throttle_until_us; // a throttled hold ends then (session_config.throttle_hold_ms); 0: none
    };

    struct ev_slot {
        uint32_t seq;
        uint32_t capture_id;
        uint64_t uptime_us;
        char kind[EVENT_KIND_MAX + 1];
        char json[EVENT_JSON_MAX + 1];
    };

    struct req {
        uint8_t op;
        uint8_t slot;    // SYNC, or the async slot to complete
        uint8_t retries; // sync ops
        uint32_t seq;
        uint32_t addr;
        uint32_t a;      // value, and-mask, poll mask, delay ms, length
        uint32_t b;      // or-bits, poll expect, call stack top
        uint32_t args[4];
        uint32_t timeout_ms;
        uint32_t interval_ms;
        esp_err_t ret;   // OP_DONE
    };

    struct pending_op {
        bool used;
        req r;
        int64_t until_us;
        int64_t next_us;
    };

    struct res {
        uint32_t seq;
        esp_err_t ret;
        uint32_t val;
    };

    struct async_slot {
        bool used;
        uint8_t op;
        uint32_t seq;
        JSValue resolve; // JS task only
        JSValue reject;
        esp_err_t ret;   // set by owner before posting
        uint32_t val;
        char why[80];    // non-empty: the call's arguments were refused (RangeError)
        bool repost;     // done, but the loop's queue was full: retry_async_posts() posts it
    };

    struct state {
        qjs_esp_arena_t *arena;
        QueueHandle_t cmd_q;
        QueueHandle_t req_q;
        QueueHandle_t res_q;
        SemaphoreHandle_t ended;     // JS task gave the runtime back
        SemaphoreHandle_t loop_lock; // owner stops it, JS task frees it
        bool ready;
        std::atomic<bool> dead;      // no scripts until reboot

        uint8_t *xfer;
        bool xfer_busy;              // JS task
        uint32_t xfer_seq;
        char *note;
        size_t note_len;
        char note_name[24];
        std::atomic<bool> note_busy;

        qjs_esp_loop_t *loop;        // under loop_lock when written
        JSModuleDef *mod;
        qjs_esp_limits_t lim;
        bool loading;                // the top level is running
        bool hook_active;            // a hook is in flight
        uint32_t seq;
        async_slot slots[MAX_ASYNC];
        std::atomic<bool> async_repost; // some slot's result waits for a repost: retry_async_posts() has work

        const uint8_t *script;
        size_t script_len;
        hook cur_hook;
        uint32_t cur_timeout_ms;
        char job_name[33];
        char variant[33];
        char family[16];

        bool network;
        JSValue results;             // log.record/check, JS task
        JSValue on_progress;         // job.onProgress callback, JS task

        std::atomic<const char *> prog_stage;
        std::atomic<int> prog_pct;
        std::atomic<bool> prog_posted;
        const char *prog_last_stage; // owner
        int64_t prog_last_us;        // owner

        bool hooks[HOOKS];
        std::atomic<bool> alive;     // the loop takes hooks
        char fail_stage[16];         // onFail arguments
        char fail_reason[96];
        char err[96];
        // The script's own error behind `err`, when it is one (else js_err_set
        // is false): onFail gets its name, message and code back.
        bool js_err_set;
        char js_err_name[24];
        char js_err_msg[96];
        char js_err_code[16];
        bool fail_js;                // onFail's err is the script's error (js_err_*)
        char results_json[RESULTS_MAX + 1];

        bool running;
        int64_t hook_end_us;         // async ops stop here

        std::atomic<uint8_t> user;   // USER_*: a job run and a session exclude each other
        bool session;                // owner sets it before any session runtime starts
        session_config cfg;
        bool rt_started;             // owner: a session runtime was started and not reaped
        std::atomic<bool> stop_wanted; // owner stopped the runtime on purpose
        uint8_t first_kind;          // what a new session runtime runs first: FIRST_*
        uint8_t *first_blob;         // heap copy, the JS task frees it
        size_t first_len;
        uint32_t first_token;
        uint32_t token;              // owner: last run/eval token
        std::atomic<uint32_t> want_token; // whose result goes to the res_ buffers
        uint32_t done_token;         // owner: last OP_DONE seen
        bool out_pending;            // owner: the last run/eval is still going
        const char *owner_note;      // owner: the last run/eval failed before reaching the JS task
        script_outcome owner_outcome;
        std::atomic<bool> rt_ending; // JS task: the session runtime's loop has returned
        std::atomic<bool> module_failed; // JS task: the module's top level threw or rejected (this runtime)
        script_outcome res_outcome;
        bool res_truncated;
        char *res_value;             // RESULT_VALUE_MAX + 1, PSRAM
        char res_error[RESULT_ERROR_MAX + 1];
        char *res_stack;             // RESULT_STACK_MAX + 1, PSRAM
        pending_op pend[MAX_ASYNC];  // owner: async ops it steps

        SemaphoreHandle_t watch_lock; // watches[] between the JS task and the owner
        watch_def watches[WATCHES];
        std::atomic<uint32_t> watch_gen;
        uint16_t watch_ids;          // JS task
        JSValue watch_fn[WATCHES];   // JS task
        engine eng;
        capture_slot caps[CAPTURES];

        SemaphoreHandle_t ev_lock;
        ev_slot *events;             // EVENTS, PSRAM ring
        uint32_t ev_seq;             // newest
        uint32_t ev_dropped;
        float ev_tokens;             // console/emit rate limit
        int64_t ev_refill_us;
        uint8_t sha256[32];
        bool have_sha;
        bool pinned;
    };

    extern state g;

    enum first_t : uint8_t { FIRST_NONE, FIRST_MODULE, FIRST_EVAL };
    enum cmd_t : uint8_t { CMD_RUN = 1, CMD_SESSION = 2 };
    enum user_t : uint8_t { USER_NONE, USER_RUN, USER_SESSION };
    bool claim(uint8_t who);
    JSContext *new_session_context(JSRuntime *rt);
    static constexpr uint32_t DHCSR = 0xE000EDF0;
    static constexpr uint32_t DEMCR = 0xE000EDFC;

    JSModuleDef *init_target_module(JSContext *ctx);
    bool add_target_globals(JSContext *ctx, JSValueConst global);
    bool add_misc_globals(JSContext *ctx, JSValueConst global);
    esp_err_t execute(const req &r, js_target *t, cancel_fn cancelled);
    bool is_quarantined(const uint8_t sha256[32]);
    void set_crash_mark(const uint8_t *sha256); // nullptr clears it
    const char *stop_text(qjs_esp_stop_t why);
    JSModuleDef *refuse_module(JSContext *ctx, const char *name, void *);
    void post_async(uint8_t slot, uint32_t seq, esp_err_t ret, uint32_t val, const char *why = nullptr);
    void retry_async_posts(); // owner task, between requests
    void stop_loop();
    int64_t now_us();
    TickType_t ms_ticks(uint32_t ms);
    void record_error(JSContext *ctx, JSValueConst e);
    void log_runtime_end(const char *what); // arena peak and device heap, before the arena is reset
    // True for an uncatchable error: the loop's own interrupt, or one the script can't recover
    // from (out of memory). Either way the runtime ends and its end answers whoever waits; for
    // the second kind this stops the loop, which nothing else would.
    bool ends_runtime(JSValueConst e);
    // Where a failure has nowhere to go (a log line, a best-effort read):
    // drop the pending exception, unless it ends the runtime (out of memory,
    // the loop's interrupt), which then stops the loop as it should.
    void drop_exception(JSContext *ctx);
    // JS task, in posted work: the loop is stopping (or qjs_esp_loop_free()
    // is running what's left). Free what the work owns; call no JS.
    bool loop_stopping();

    bool session_alloc();
    bool result_pending(); // a run or eval was started and its result hasn't come
    bool init_agent_module(JSContext *ctx);
    bool add_agent_global(JSContext *ctx, JSValueConst global);
    void run_session_runtime();
    void session_console(int level, const char *line);
    void emit_event(const char *kind, const char *json, uint32_t capture_id, bool limited);
    size_t json_str(char *out, size_t cap, const char *s, size_t max_in = (size_t)-1);
    size_t error_json(JSContext *ctx, JSValueConst e, const char *what, char *out, size_t cap);
    JSValue new_watch(JSContext *ctx, JSValueConst spec, JSValueConst handler);
    JSValue list_watches(JSContext *ctx);
    void clear_watches_js(JSContext *ctx);
    void watch_cb(JSContext *ctx, void *arg);
    void watch_poll(js_target &t);
    void watch_verdict(const req &r, js_target &t);
    void watch_before_op(uint8_t op, uint32_t addr, js_target &t);
    void watch_after_op(uint8_t op, uint32_t addr, esp_err_t ret);
    void watch_reset_engine();
    void watch_disarm(js_target &t);
    void async_done(JSContext *ctx, void *arg);
    void async_reset(JSContext *ctx);
    void xfer_seen(uint32_t seq);
    bool note_claim(JSContext *ctx, JSValueConst data, const char *what);
    void incident_from_script(const req &r);
    void incident_data(const req &r);

    bool init_misc_modules(JSContext *ctx, bool session);
    void progress_cb(JSContext *ctx, void *arg);
    JSValue target_error(JSContext *ctx, esp_err_t err, const char *code, const char *what);
    const char *code_of(esp_err_t err);
    JSValue async_call(JSContext *ctx, req &r);
    JSValue check_owned(JSContext *ctx, uint8_t op);
    const char *op_name(uint8_t op);
    int opt_u32(JSContext *ctx, JSValueConst opts, const char *name, uint32_t dflt, uint32_t *out);
    int get_bytes(JSContext *ctx, JSValueConst v, const uint8_t **p, size_t *len);
} // namespace qjs_binder::priv
