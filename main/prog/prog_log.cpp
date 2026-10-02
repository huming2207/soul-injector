#include "prog_log.hpp"

#include <cstring>
#include <new>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <on9rstore.hpp>
#include <pb_encode.h>

esp_err_t prog_log::init()
{
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    auto ret = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, PARTITION_LABEL, &mount_cfg, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init: failed to mount %s: 0x%x %s", BASE_PATH, ret, esp_err_to_name(ret));
        return ret;
    }

    // Boot records stay small: a coredump copy would not fit a LogEntry anyway.
    on9rstore_cfg cfg = {};
    cfg.copy_coredump = false;
    cfg.protect_unacked = true;
    store = new (std::nothrow) on9rstore(BASE_PATH, &cfg);
    if (store == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ret = store->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init: store failed: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "init: newest %llu, acked %llu", store->get_newest_entry_id(), store->get_acked_entry_id());
    return ESP_OK;
}

esp_err_t prog_log::check_space()
{
    if (write_failed.load()) {
        return ESP_FAIL;
    }
    return store->can_append(si_manage_RunRecord_size) ? ESP_OK : ESP_ERR_NO_MEM;
}

void prog_log::add_run(const si_manage_RunRecord &record)
{
    uint8_t encoded[si_manage_RunRecord_size] = {};
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    esp_err_t ret = pb_encode(&stream, si_manage_RunRecord_fields, &record) ? ESP_OK : ESP_FAIL;
    ret = ret ?: store->append_entry(ENTRY_RUN, encoded, stream.bytes_written, nullptr, portMAX_DELAY, true);
    if (ret != ESP_OK) {
        // The store state is uncertain: stop programming until a restart recovers it
        ESP_LOGE(TAG, "add_run: run %lu not recorded: 0x%x %s", record.result.run_id, ret, esp_err_to_name(ret));
        write_failed.store(true);
    }
}

esp_err_t prog_log::set_time(uint64_t utc_ms)
{
    if (utc_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (!time_set) {
        on9rstore_def::time_anchor anchor = {};
        anchor.source_mask = on9rstore_def::TIME_SOURCE_SERVER_ESTIMATE;
        anchor.source_count = 1;
        anchor.quality = on9rstore_def::TIME_ANCHOR_QUALITY_PROVISIONAL;
        anchor.monotonic_us = esp_timer_get_time();
        anchor.utc_us = utc_ms * 1000;
        anchor.uncertainty_us = 1000000; // NTP through the host plus the USB round trip
        ret = store->append_time_anchor(anchor);
        time_set = ret == ESP_OK;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_time: failed: 0x%x %s", ret, esp_err_to_name(ret));
    }
    return ret;
}

void prog_log::convert_boot_record(si_manage_LogEntry &entry)
{
    on9rstore_def::boot_event event = {};
    if (entry.record.size < sizeof(event)) {
        entry.type = si_manage_LogEntryType_LOG_ENTRY_UNKNOWN;
        entry.record.size = 0;
        return;
    }

    memcpy(&event, entry.record.bytes, sizeof(event));
    si_manage_BootRecord boot = si_manage_BootRecord_init_zero;
    boot.reset_reason = event.reset_reason;
    pb_ostream_t stream = pb_ostream_from_buffer(entry.record.bytes, sizeof(entry.record.bytes));
    pb_encode(&stream, si_manage_BootRecord_fields, &boot);
    entry.record.size = stream.bytes_written;
}

esp_err_t prog_log::read_entry(uint64_t after_id, si_manage_LogEntry &entry)
{
    on9rstore_def::entry_range_cursor cursor = {};
    cursor.next_entry_id = after_id + 1;
    on9rstore_def::entry_header header = {};
    auto ret = store->read_next_entry(&cursor, entry.record.bytes, sizeof(entry.record.bytes), &header);
    // Too large for a LogEntry, or corrupt: reported as UNKNOWN so the host can acknowledge past it
    bool readable_header = (ret == ESP_ERR_INVALID_SIZE || ret == ESP_ERR_INVALID_CRC) && header.entry_id != 0;
    if (ret != ESP_OK && !readable_header) {
        return ret;
    }

    entry.id = header.entry_id;
    entry.uptime_us = header.uptime_us;
    entry.record.size = ret == ESP_OK ? header.len : 0;
    if (ret == ESP_OK && header.type == ENTRY_RUN) {
        entry.type = si_manage_LogEntryType_LOG_ENTRY_RUN;
    } else if (ret == ESP_OK && header.type == on9rstore_def::ENTRY_BOOT_EVENT) {
        entry.type = si_manage_LogEntryType_LOG_ENTRY_BOOT;
        convert_boot_record(entry);
    } else {
        entry.type = si_manage_LogEntryType_LOG_ENTRY_UNKNOWN;
        entry.record.size = 0;
    }

    on9rstore_def::entry_utc_info utc = {};
    if (store->get_entry_utc(header, &utc) == ESP_OK) {
        entry.utc_ms = utc.utc_us / 1000;
    }
    return ESP_OK;
}

esp_err_t prog_log::read(uint64_t after_id, pb_ostream_t &out)
{
    // Tag plus the longest length prefix of one LogEntry.
    constexpr size_t entry_overhead = 1 + 2;
    while (true) {
        si_manage_LogEntry entry = si_manage_LogEntry_init_zero;
        auto ret = read_entry(after_id, entry);
        if (ret == ESP_ERR_NOT_FOUND) {
            return ESP_OK;
        }
        if (ret != ESP_OK) {
            return out.bytes_written > 0 ? ESP_OK : ret;
        }

        size_t entry_len = 0;
        if (!pb_get_encoded_size(&entry_len, si_manage_LogEntry_fields, &entry) || entry_len + entry_overhead > out.max_size - out.bytes_written) {
            return ESP_OK; // The host asks again from the last ID it got.
        }

        if (!pb_encode_tag(&out, PB_WT_STRING, si_manage_LogReadResponse_entries_tag) ||
            !pb_encode_submessage(&out, si_manage_LogEntry_fields, &entry)) {
            ESP_LOGE(TAG, "read: encode failed: %s", PB_GET_ERROR(&out));
            return ESP_FAIL;
        }
        after_id = entry.id;
    }
}

esp_err_t prog_log::ack(uint64_t up_to_id)
{
    return store->set_acked_entry_id(up_to_id);
}

void prog_log::get_info(si_manage_DeviceInfo &info)
{
    info.log_newest_id = store->get_newest_entry_id();
    info.log_acked_id = store->get_acked_entry_id();
    info.log_full = check_space() != ESP_OK;
}
