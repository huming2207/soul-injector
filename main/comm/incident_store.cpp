#include "incident_store.hpp"

#if CONFIG_SI_JS_ENABLE

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <esp_log.h>
#include <nvs.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    constexpr char NVS_NAMESPACE[] = "si_agent";
    constexpr char NVS_NEXT_KEY[] = "inc_next";
    constexpr uint32_t TOTAL_OFFSET = 12;

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

esp_err_t incident_store::init()
{
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
        if (stat(path, &st) != 0 || st.st_size < (off_t)HEADER_SIZE) {
            unlink(path);
            continue;
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
        entries[n++] = {id, (uint32_t)st.st_size};
        bytes += st.st_size;
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

void incident_store::save_next_id()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, NVS_NEXT_KEY, next_id);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

uint32_t incident_store::create(const header &h)
{
    if (!ready) {
        return 0;
    }
    make_room(0, HEADER_SIZE, true);
    uint8_t hdr[HEADER_SIZE] = {'S', 'I', 'I', 'N', 1, h.kind};
    const uint32_t id = next_id;
    put16(hdr + 6, h.watch_id);
    put32(hdr + 8, id);
    put32(hdr + TOTAL_OFFSET, HEADER_SIZE);
    put32(hdr + 16, h.boot);
    put64(hdr + 24, h.uptime_us);
    put64(hdr + 32, h.utc_ms);
    strlcpy(reinterpret_cast<char *>(hdr + 40), h.name ? h.name : "", 24);
    strlcpy(reinterpret_cast<char *>(hdr + 64), h.trap ? h.trap : "", 24);

    char path[32];
    path_of(id, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    bool ok = f != nullptr && fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    ok = f != nullptr && fclose(f) == 0 && ok;
    if (!ok) {
        ESP_LOGE(TAG, "create: can't write %s", path);
        unlink(path);
        return 0;
    }
    next_id++;
    save_next_id();
    entries[n++] = {id, (uint32_t)HEADER_SIZE};
    bytes += HEADER_SIZE;
    return id;
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

size_t incident_store::list(uint32_t after_id, si_agent_IncidentInfo *out, size_t max, bool *more)
{
    size_t got = 0;
    *more = false;
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
        strlcpy(e.name, reinterpret_cast<const char *>(hdr + 40), std::min(sizeof(e.name), (size_t)24));
        strlcpy(e.trap, reinterpret_cast<const char *>(hdr + 64), std::min(sizeof(e.trap), (size_t)24));
    }
    return got;
}

esp_err_t incident_store::read(uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, size_t *len, uint32_t *total)
{
    int i = find(id);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *total = entries[i].size;
    *len = 0;
    if (offset >= *total) {
        return offset == *total ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    char path[32];
    path_of(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return ESP_FAIL;
    }
    const size_t want = std::min<size_t>(cap, *total - offset);
    bool ok = fseek(f, offset, SEEK_SET) == 0 && fread(buf, 1, want, f) == want;
    fclose(f);
    *len = ok ? want : 0;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t incident_store::remove(uint32_t id)
{
    if (id == 0) {
        while (n) {
            drop(n - 1);
        }
        return ESP_OK;
    }
    int i = find(id);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    drop(i);
    return ESP_OK;
}

#endif
