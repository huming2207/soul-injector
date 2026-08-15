// This TU hosts the rapidyaml implementation for the whole firmware
// (single-header library: exactly one TU must define this).
#define RYML_SINGLE_HDR_DEFINE_NOW
#include "yaml_doc.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "yaml_doc";

/** Sanity cap so a corrupt file cannot request an absurd allocation. */
static constexpr size_t MAX_YAML_FILE_SIZE = 2 * 1024 * 1024;

// -------------------------------------------------------------------
// PSRAM-backed ryml callbacks
// -------------------------------------------------------------------

static void *yaml_alloc_cb(size_t len, void * /*hint*/, void * /*user_data*/)
{
    if (len == 0) {
        len = 1;
    }
    return heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void yaml_free_cb(void *mem, size_t /*size*/, void * /*user_data*/)
{
    heap_caps_free(mem);
}

void yaml_install_psram_callbacks()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;

    // Keep the stock error handlers, but route allocations to PSRAM.
    ryml::Callbacks cbs = ryml::get_callbacks();
    cbs.set_allocate(yaml_alloc_cb);
    cbs.set_free(yaml_free_cb);
    ryml::set_callbacks(cbs);
    ESP_LOGI(TAG, "ryml callbacks installed (PSRAM backed)");
}

// -------------------------------------------------------------------
// Scalar parsing helpers
// -------------------------------------------------------------------

static esp_err_t scalar_to_u32(ryml::csubstr val, const char *ctx, uint32_t *out)
{
    if (val.empty() || val.len > 31) {
        ESP_LOGE(TAG, "%s: value is empty or too long (%zu chars)", ctx, val.len);
        return ESP_ERR_INVALID_ARG;
    }

    char buf[32] = {};
    memcpy(buf, val.str, val.len);
    buf[val.len] = '\0';

    // Trim trailing whitespace so "0x1000 \n" style block scalars still parse.
    size_t end = strlen(buf);
    while (end > 0 && (buf[end - 1] == ' ' || buf[end - 1] == '\t' || buf[end - 1] == '\r' || buf[end - 1] == '\n')) {
        buf[--end] = '\0';
    }
    if (end == 0) {
        ESP_LOGE(TAG, "%s: value is blank", ctx);
        return ESP_ERR_INVALID_ARG;
    }

    // Values are unsigned; a minus sign would silently wrap via strtoul.
    if (buf[0] == '-') {
        ESP_LOGE(TAG, "%s: negative value '%s' is not allowed", ctx, buf);
        return ESP_ERR_INVALID_ARG;
    }

    errno = 0;
    char *parse_end = nullptr;
    // Base 0 keeps YAML 1.1 semantics: 0x.. hex, 0.. octal, decimal otherwise.
    unsigned long parsed = strtoul(buf, &parse_end, 0);
    if (parse_end == buf || *parse_end != '\0') {
        ESP_LOGE(TAG, "%s: '%s' is not a valid number", ctx, buf);
        return ESP_ERR_INVALID_ARG;
    }
    if (errno == ERANGE || parsed > UINT32_MAX) {
        ESP_LOGE(TAG, "%s: value '%s' out of 32-bit range", ctx, buf);
        return ESP_ERR_INVALID_ARG;
    }

    *out = static_cast<uint32_t>(parsed);
    return ESP_OK;
}

esp_err_t yaml_doc::node_u32(ryml::ConstNodeRef val_node, const char *ctx, uint32_t *out)
{
    if (val_node.invalid() || !val_node.has_val()) {
        ESP_LOGE(TAG, "%s: node is missing or not a scalar", ctx);
        return ESP_ERR_INVALID_ARG;
    }
    return scalar_to_u32(val_node.val(), ctx, out);
}

esp_err_t yaml_doc::get_u32(ryml::ConstNodeRef node, const char *key, uint32_t *out)
{
    if (key == nullptr || node.invalid() || !node.is_map() || !node.has_child(key)) {
        ESP_LOGE(TAG, "missing required key '%s'", key ? key : "?");
        return ESP_ERR_INVALID_ARG;
    }
    char ctx[64];
    snprintf(ctx, sizeof(ctx), "key '%s'", key);
    return node_u32(node[key], ctx, out);
}

esp_err_t yaml_doc::get_opt_u32(ryml::ConstNodeRef node, const char *key, std::optional<uint32_t> &out)
{
    out.reset();
    if (node.invalid() || !node.is_map() || !node.has_child(key)) {
        return ESP_OK; // Absent is fine for optional fields.
    }
    uint32_t tmp = 0;
    auto ret = get_u32(node, key, &tmp);
    if (ret != ESP_OK) {
        return ret;
    }
    out = tmp;
    return ESP_OK;
}

static esp_err_t scalar_to_bool(ryml::csubstr val, const char *ctx, bool *out)
{
    if (val == "true" || val == "1" || val == "yes") {
        *out = true;
        return ESP_OK;
    }
    if (val == "false" || val == "0" || val == "no") {
        *out = false;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s: '%.*s' is not a valid boolean", ctx, (int)val.len, val.str);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t yaml_doc::get_bool(ryml::ConstNodeRef node, const char *key, bool *out)
{
    if (key == nullptr || node.invalid() || !node.is_map() || !node.has_child(key)) {
        ESP_LOGE(TAG, "missing required key '%s'", key ? key : "?");
        return ESP_ERR_INVALID_ARG;
    }
    char ctx[64];
    snprintf(ctx, sizeof(ctx), "key '%s'", key);
    return scalar_to_bool(node[key].val(), ctx, out);
}

esp_err_t yaml_doc::get_opt_bool(ryml::ConstNodeRef node, const char *key, std::optional<bool> &out)
{
    out.reset();
    if (node.invalid() || !node.is_map() || !node.has_child(key)) {
        return ESP_OK;
    }
    bool tmp = false;
    auto ret = get_bool(node, key, &tmp);
    if (ret != ESP_OK) {
        return ret;
    }
    out = tmp;
    return ESP_OK;
}

esp_err_t yaml_doc::get_str(ryml::ConstNodeRef node, const char *key, char *out, size_t out_len)
{
    if (out == nullptr || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (key == nullptr || node.invalid() || !node.is_map() || !node.has_child(key)) {
        ESP_LOGE(TAG, "missing required key '%s'", key ? key : "?");
        return ESP_ERR_INVALID_ARG;
    }

    ryml::csubstr val = node[key].val();
    if (val.empty()) {
        ESP_LOGE(TAG, "key '%s' is empty", key);
        return ESP_ERR_INVALID_ARG;
    }
    if (val.len >= out_len) {
        ESP_LOGE(TAG, "key '%s' value (%zu chars) does not fit in %zu bytes", key, val.len, out_len);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, val.str, val.len);
    out[val.len] = '\0';
    return ESP_OK;
}

bool yaml_doc::has_tag(ryml::ConstNodeRef node, const char *tag)
{
    return node.has_val_tag() && node.val_tag() == ryml::to_csubstr(tag);
}

// -------------------------------------------------------------------
// Document lifecycle
// -------------------------------------------------------------------

yaml_doc::~yaml_doc()
{
    release();
}

void yaml_doc::release()
{
    tree = {}; // tree nodes reference the buffer, so clear it first
    if (buf != nullptr) {
        heap_caps_free(buf);
        buf = nullptr;
    }
    loaded = false;
}

esp_err_t yaml_doc::load(const char *path, yaml_doc &out)
{
    yaml_install_psram_callbacks();

    if (path == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = nullptr;
    int retry = 5;
    while (retry-- > 0) {
        file = fopen(path, "rb");
        if (file != nullptr) {
            break;
        }
        ESP_LOGW(TAG, "load: failed to open %s, retrying... (%d left)", path, retry);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (file == nullptr) {
        ESP_LOGE(TAG, "load: cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        ESP_LOGE(TAG, "load: ftell failed on %s", path);
        return ESP_FAIL;
    }
    fseek(file, 0, SEEK_SET);

    if (static_cast<size_t>(file_size) > MAX_YAML_FILE_SIZE) {
        fclose(file);
        ESP_LOGE(TAG, "load: %s (%ld bytes) exceeds the %zu byte limit", path, file_size, MAX_YAML_FILE_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    out.release();
    out.buf = static_cast<char *>(heap_caps_calloc(1, static_cast<size_t>(file_size) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (out.buf == nullptr) {
        fclose(file);
        ESP_LOGE(TAG, "load: cannot allocate %ld bytes for %s", file_size, path);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(out.buf, 1, static_cast<size_t>(file_size), file);
    fclose(file);
    if (read_bytes != static_cast<size_t>(file_size) && file_size > 0) {
        ESP_LOGE(TAG, "load: read %zu of %ld bytes from %s", read_bytes, file_size, path);
        out.release();
        return ESP_FAIL;
    }

    try {
        out.tree = ryml::parse_in_place(ryml::substr(out.buf, read_bytes));
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "load: YAML parse exception in %s: %s", path, e.what());
        out.release();
        return ESP_FAIL;
    } catch (...) {
        ESP_LOGE(TAG, "load: unknown YAML parse exception in %s", path);
        out.release();
        return ESP_FAIL;
    }

    if (!out.tree.rootref().is_map()) {
        ESP_LOGE(TAG, "load: YAML root of %s is not a map", path);
        out.release();
        return ESP_ERR_INVALID_STATE;
    }

    out.loaded = true;
    return ESP_OK;
}
