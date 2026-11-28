#include "incident_store.hpp"

#if CONFIG_SI_JS_ENABLE

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <nvs.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    constexpr char NVS_NAMESPACE[] = "si_agent";
    constexpr char NVS_NEXT_KEY[] = "inc_next";
    constexpr uint32_t TOTAL_OFFSET = 12;

    void copy_field(char *dst, size_t dst_size, const uint8_t *field, size_t field_len)
    {
        const size_t len = strnlen(reinterpret_cast<const char *>(field), std::min(field_len, dst_size - 1));
        memcpy(dst, field, len);
        dst[len] = '\0';
    }

    void put16(uint8_t *p, uint16_t v)
    {
        p[0] = v;
        p[1] = v >> 8;
    }

    void put32(uint8_t *p, uint32_t v)
    {
        for (int i = 0; i < 4; i++) {
            p[i] = v >> (8 * i);
        }
    }

    void put64(uint8_t *p, uint64_t v)
    {
        for (int i = 0; i < 8; i++) {
            p[i] = v >> (8 * i);
        }
    }

    uint32_t get32(const uint8_t *p)
    {
        return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    }

    uint64_t get64(const uint8_t *p)
    {
        return get32(p) | (uint64_t)get32(p + 4) << 32;
    }

    void path_of(uint32_t id, char *out, size_t cap)
    {
        snprintf(out, cap, "/data/inc/%08lu.sin", (unsigned long)id);
    }

    bool id_of(const char *name, uint32_t *id)
    {
        unsigned long v = 0;
        char tail[8] = {};
        return strlen(name) == 12 && sscanf(name, "%8lu.%3s", &v, tail) == 2 && strcasecmp(tail, "sin") == 0 && v != 0 &&
               (*id = v, true);
    }
} // namespace

void incident_store::lock() const
{
    xSemaphoreTake(mutex, portMAX_DELAY);
}

void incident_store::unlock() const
{
    xSemaphoreGive(mutex);
}

esp_err_t incident_store::init()
{
    mutex = xSemaphoreCreateMutex();
    queue = xQueueCreate(QUEUE_LEN, sizeof(job *));
    if (mutex == nullptr || queue == nullptr) {
        ESP_LOGE(TAG, "init: out of memory");
        return ESP_ERR_NO_MEM;
    }
    if (mkdir(DIR_PATH, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "init: can't make %s: %d", DIR_PATH, errno);
        return ESP_FAIL;
    }
    DIR *dir = opendir(DIR_PATH);
    if (dir == nullptr) {
        return ESP_FAIL;
    }
    uint32_t max_id = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
        uint32_t id = 0;
        char path[32];
        struct stat st = {};
        if (!id_of(de->d_name, &id)) {
            continue;
        }
        path_of(id, path, sizeof(path));
        // The header's total, not the file's length: append() writes a section and
        // then the total, so a reset in between leaves a partial section at the
        // end, and readers reject an incident whose total isn't its length.
        uint8_t hdr[TOTAL_OFFSET + 4] = {};
        FILE *f = stat(path, &st) == 0 ? fopen(path, "rb") : nullptr;
        bool ok = f != nullptr && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
        if (f != nullptr) {
            fclose(f);
        }
        // off_t is 32-bit signed here: compare sizes as uint64_t, or a damaged
        // total of 2 GiB or more would pass as negative
        const uint32_t total = get32(hdr + TOTAL_OFFSET);
        const uint64_t file_size = st.st_size > 0 ? static_cast<uint64_t>(st.st_size) : 0;
        if (!ok || memcmp(hdr, "SIIN", 4) != 0 || hdr[4] != 1 || total < HEADER_SIZE || total > file_size) {
            unlink(path);
            continue;
        }
        if (total < file_size) {
            ESP_LOGW(TAG, "init: incident %lu ends in a partial section; keeping its first %lu bytes", (unsigned long)id,
                     (unsigned long)total);
            truncate(path, total);
        }
        max_id = std::max(max_id, id);
        if (n == CONFIG_SI_AGENT_INCIDENTS_MAX) {
            auto oldest = std::min_element(entries, entries + n, [](const entry &a, const entry &b) { return a.id < b.id; });
            if (oldest->id > id) {
                unlink(path);
                continue;
            }
            drop(oldest - entries);
        }
        entries[n++] = {id, total};
        bytes += total;
    }
    closedir(dir);
    std::sort(entries, entries + n, [](const entry &a, const entry &b) { return a.id < b.id; });

    nvs_handle_t nvs;
    uint32_t saved = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, NVS_NEXT_KEY, &saved);
        nvs_close(nvs);
    }
    next_id = std::max({saved, max_id + 1, (uint32_t)1});
    make_room(0, 0, false);
    // Below the SIDP service task (11), the flasher (10) and the power sampler (9).
    if (xTaskCreate(writer_task, "incidents", 6144, this, tskIDLE_PRIORITY + 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "init: can't start the writer task");
        return ESP_ERR_NO_MEM;
    }
    ready = true;
    ESP_LOGI(TAG, "%u incidents, %u bytes, next %lu", (unsigned)n, (unsigned)bytes, (unsigned long)next_id);
    return ESP_OK;
}

int incident_store::find(uint32_t id) const
{
    for (size_t i = 0; i < n; i++) {
        if (entries[i].id == id) {
            return i;
        }
    }
    return -1;
}

void incident_store::drop(size_t index)
{
    char path[32];
    path_of(entries[index].id, path, sizeof(path));
    unlink(path);
    bytes -= std::min<size_t>(bytes, entries[index].size);
    memmove(entries + index, entries + index + 1, (n - index - 1) * sizeof(entry));
    n--;
}

void incident_store::make_room(uint32_t keep_id, size_t need, bool new_entry)
{
    size_t i = 0;
    while (i < n && ((new_entry && n >= CONFIG_SI_AGENT_INCIDENTS_MAX) || bytes + need > MAX_BYTES)) {
        if (entries[i].id == keep_id) {
            i++;
            continue;
        }
        drop(i);
    }
}

void incident_store::save_next_id(uint32_t value)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, NVS_NEXT_KEY, value);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

incident_store::job *incident_store::make_job(const part *parts, size_t count)
{
    if (count > MAX_PARTS) {
        return nullptr;
    }
    size_t payload = 0;
    for (size_t i = 0; i < count; i++) {
        payload += parts[i].len;
    }
    const size_t alloc = sizeof(job) + payload;
    // Room among the queued writes: the writer frees it as it goes.
    const int64_t until = esp_timer_get_time() + ENQUEUE_WAIT_MS * 1000LL;
    while (pending_bytes.load() + alloc > PENDING_MAX && esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (pending_bytes.load() + alloc > PENDING_MAX) {
        ESP_LOGW(TAG, "%u bytes of incident writes still queued: dropping %u more", (unsigned)pending_bytes.load(), (unsigned)alloc);
        return nullptr;
    }
    auto *j = static_cast<job *>(heap_caps_malloc(alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (j == nullptr) {
        j = static_cast<job *>(heap_caps_malloc(alloc, MALLOC_CAP_8BIT));
    }
    if (j == nullptr) {
        ESP_LOGW(TAG, "out of memory for a %u-byte incident write", (unsigned)alloc);
        return nullptr;
    }
    memset(j, 0, sizeof(job));
    j->alloc = alloc;
    j->count = count;
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        j->parts[i] = {parts[i].type, off, parts[i].len};
        if (parts[i].len) {
            memcpy(j->bytes() + off, parts[i].data, parts[i].len);
        }
        off += parts[i].len;
    }
    j->queued_us = esp_timer_get_time();
    return j;
}

bool incident_store::enqueue(job *j)
{
    pending_bytes += j->alloc;
    if (xQueueSend(queue, &j, pdMS_TO_TICKS(ENQUEUE_WAIT_MS)) != pdTRUE) {
        pending_bytes -= j->alloc;
        ESP_LOGW(TAG, "%u incident writes still queued: dropping one for incident %lu", (unsigned)QUEUE_LEN, (unsigned long)j->id);
        heap_caps_free(j);
        return false;
    }
    return true;
}

uint32_t incident_store::open(const header &h, const part *parts, size_t count)
{
    if (!ready) {
        return 0;
    }
    job *j = make_job(parts, count);
    if (j == nullptr) {
        return 0;
    }
    j->create = true;
    j->kind = h.kind;
    j->watch_id = h.watch_id;
    strlcpy(j->name, h.name ? h.name : "", sizeof(j->name));
    strlcpy(j->trap, h.trap ? h.trap : "", sizeof(j->trap));
    j->boot = h.boot;
    j->uptime_us = h.uptime_us;
    j->utc_ms = h.utc_ms;
    lock();
    j->id = next_id++;
    unlock();
    const uint32_t id = j->id;
    return enqueue(j) ? id : 0;
}

esp_err_t incident_store::add(uint32_t id, uint8_t type, const void *data, size_t len)
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (id == 0) { // open() didn't record it
        return ESP_ERR_NOT_FOUND;
    }
    const part p = {type, data, len};
    job *j = make_job(&p, 1);
    if (j == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    j->id = id;
    return enqueue(j) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void incident_store::writer_task(void *arg)
{
    auto *self = static_cast<incident_store *>(arg);
    for (;;) {
        job *j = nullptr;
        if (xQueueReceive(self->queue, &j, portMAX_DELAY) != pdTRUE || j == nullptr) {
            continue;
        }
        self->write(*j);
        self->pending_bytes -= j->alloc;
        heap_caps_free(j);
    }
}

void incident_store::write(job &j)
{
    const int64_t start = esp_timer_get_time();
    if (!j.create) {
        lock();
        const esp_err_t ret = append(j.id, j.parts[0].type, j.bytes() + j.parts[0].offset, j.parts[0].len);
        unlock();
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) { // not found: deleted meanwhile
            ESP_LOGW(TAG, "incident %lu: section %u not written: %s", (unsigned long)j.id, j.parts[0].type, esp_err_to_name(ret));
        }
        return;
    }
    uint32_t total = 0;
    lock();
    const bool ok = write_new(j, &total);
    const uint32_t next = next_id;
    unlock();
    if (!ok) {
        return;
    }
    save_next_id(next); // outside the lock: NVS isn't the files
    ESP_LOGI(TAG, "incident %lu written in %lld ms (%lu bytes, queued %lld ms)", (unsigned long)j.id, (esp_timer_get_time() - start) / 1000,
             (unsigned long)total, (start - j.queued_us) / 1000);
}

bool incident_store::write_new(job &j, uint32_t *total)
{
    // All sections in one open: a FAT update per file instead of per section.
    // The header's total is written last, so a reset part way leaves a file
    // that init() cuts back to its header.
    size_t keep = 0, size = HEADER_SIZE;
    while (keep < j.count && size + 8 + ((j.parts[keep].len + 3) & ~(size_t)3) <= MAX_BYTES) {
        size += 8 + ((j.parts[keep].len + 3) & ~(size_t)3);
        keep++;
    }
    if (keep < j.count) {
        ESP_LOGW(TAG, "incident %lu would pass the space limit: %u of its %u sections kept", (unsigned long)j.id, (unsigned)keep,
                 (unsigned)j.count);
    }
    make_room(0, size, true);

    uint8_t hdr[HEADER_SIZE] = {'S', 'I', 'I', 'N', 1, j.kind};
    put16(hdr + 6, j.watch_id);
    put32(hdr + 8, j.id);
    put32(hdr + TOTAL_OFFSET, HEADER_SIZE);
    put32(hdr + 16, j.boot);
    put64(hdr + 24, j.uptime_us);
    put64(hdr + 32, j.utc_ms);
    strlcpy(reinterpret_cast<char *>(hdr + 40), j.name, 24);
    strlcpy(reinterpret_cast<char *>(hdr + 64), j.trap, 24);

    char path[32];
    path_of(j.id, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    bool ok = f != nullptr && fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    static const uint8_t zeros[3] = {};
    for (size_t i = 0; ok && i < keep; i++) {
        const auto &p = j.parts[i];
        const size_t padded = (p.len + 3) & ~(size_t)3;
        uint8_t sec[8] = {p.type};
        put32(sec + 4, p.len);
        ok = fwrite(sec, 1, 8, f) == 8 && (p.len == 0 || fwrite(j.bytes() + p.offset, 1, p.len, f) == p.len) &&
             fwrite(zeros, 1, padded - p.len, f) == padded - p.len;
    }
    uint8_t total_le[4];
    put32(total_le, size);
    ok = ok && fseek(f, TOTAL_OFFSET, SEEK_SET) == 0 && fwrite(total_le, 1, 4, f) == 4;
    ok = f != nullptr && fclose(f) == 0 && ok;
    if (!ok) {
        ESP_LOGE(TAG, "write: can't write %s", path);
        unlink(path);
        return false;
    }
    entries[n++] = {j.id, (uint32_t)size};
    bytes += size;
    *total = size;
    return true;
}

esp_err_t incident_store::append(uint32_t id, uint8_t type, const void *data, size_t len)
{
    if (find(id) < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t padded = (len + 3) & ~(size_t)3;
    make_room(id, 8 + padded, false);
    int i = find(id);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (bytes + 8 + padded > MAX_BYTES) {
        ESP_LOGW(TAG, "append: incident %lu would pass the space limit, section %u dropped", (unsigned long)id, type);
        return ESP_ERR_NO_MEM;
    }
    char path[32];
    path_of(id, path, sizeof(path));
    FILE *f = fopen(path, "r+b");
    if (f == nullptr) {
        return ESP_FAIL;
    }
    uint8_t sec[8] = {type};
    put32(sec + 4, len);
    static const uint8_t zeros[3] = {};
    const uint32_t total = entries[i].size + 8 + padded;
    uint8_t total_le[4];
    put32(total_le, total);
    bool ok = fseek(f, entries[i].size, SEEK_SET) == 0 && fwrite(sec, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) &&
              fwrite(zeros, 1, padded - len, f) == padded - len && fseek(f, TOTAL_OFFSET, SEEK_SET) == 0 &&
              fwrite(total_le, 1, 4, f) == 4;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        ESP_LOGE(TAG, "append: can't write %s", path);
        return ESP_FAIL;
    }
    bytes += total - entries[i].size;
    entries[i].size = total;
    return ESP_OK;
}

uint32_t incident_store::count() const
{
    if (mutex == nullptr) {
        return 0;
    }
    lock();
    const uint32_t c = n;
    unlock();
    return c;
}

uint32_t incident_store::newest() const
{
    if (mutex == nullptr) {
        return 0;
    }
    lock();
    const uint32_t id = n ? entries[n - 1].id : 0;
    unlock();
    return id;
}

size_t incident_store::list(uint32_t after_id, si_agent_IncidentInfo *out, size_t max, bool *more)
{
    size_t got = 0;
    *more = false;
    if (!ready) {
        return 0;
    }
    lock();
    for (size_t i = 0; i < n; i++) {
        if (entries[i].id <= after_id) {
            continue;
        }
        if (got == max) {
            *more = true;
            break;
        }
        char path[32];
        uint8_t hdr[HEADER_SIZE];
        path_of(entries[i].id, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        bool ok = f != nullptr && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && memcmp(hdr, "SIIN", 4) == 0;
        if (f != nullptr) {
            fclose(f);
        }
        if (!ok) {
            continue;
        }
        auto &e = out[got++];
        e = si_agent_IncidentInfo_init_zero;
        e.id = entries[i].id;
        e.size = entries[i].size;
        e.kind = hdr[5];
        e.boot = get32(hdr + 16);
        e.uptime_us = get64(hdr + 24);
        e.utc_ms = get64(hdr + 32);
        // From the file: a damaged header may lack the NUL, so never read past the field
        copy_field(e.name, sizeof(e.name), hdr + 40, 24);
        copy_field(e.trap, sizeof(e.trap), hdr + 64, 24);
    }
    unlock();
    return got;
}

esp_err_t incident_store::read(uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, size_t *len, uint32_t *total)
{
    *len = 0;
    if (!ready) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    int i = find(id);
    if (i < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *total = entries[i].size;
    if (offset >= *total) {
        unlock();
        return offset == *total ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    char path[32];
    path_of(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    bool ok = f != nullptr;
    const size_t want = std::min<size_t>(cap, *total - offset);
    ok = ok && fseek(f, offset, SEEK_SET) == 0 && fread(buf, 1, want, f) == want;
    if (f != nullptr) {
        fclose(f);
    }
    unlock();
    *len = ok ? want : 0;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t incident_store::remove(uint32_t id)
{
    if (!ready) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    esp_err_t ret = ESP_OK;
    if (id == 0) {
        while (n) {
            drop(n - 1);
        }
    } else if (int i = find(id); i >= 0) {
        drop(i);
    } else {
        ret = ESP_ERR_NOT_FOUND;
    }
    unlock();
    return ret;
}

#endif
