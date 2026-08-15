#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <esp_err.h>
#include <ryml.hpp>

/**
 * YAML document wrapper around rapidyaml.
 *
 *  - loads the file into one heap_caps_calloc()'d PSRAM buffer, which this
 *    object owns and frees again (RAII); the parsed tree references the
 *    buffer, so copied-out strings must not outlive the yaml_doc;
 *  - every getter VALIDATES: missing keys, malformed numbers and bad node
 *    shapes are loud errors that name the offending key, instead of silently
 *    producing 0 like the old parse helpers did;
 *  - optional fields use std::optional so "absent" is distinguishable from
 *    a legitimate zero value.
 */
class yaml_doc
{
public:
    yaml_doc() = default;
    ~yaml_doc();

    yaml_doc(yaml_doc const &) = delete;
    yaml_doc &operator=(yaml_doc const &) = delete;

    /** Load and parse a YAML file. The file buffer is freed on destruction. */
    static esp_err_t load(const char *path, yaml_doc &out);

    /** Free the file buffer and drop the tree early. */
    void release();

    ryml::ConstNodeRef root() const
    {
        return tree.rootref();
    }

    bool valid() const
    {
        return loaded;
    }

    // ---------------------------------------------------------------
    // Validating scalar getters (node map + key based)
    // ---------------------------------------------------------------

    /** Required unsigned 32-bit field. Errors when missing or malformed. */
    static esp_err_t get_u32(ryml::ConstNodeRef node, const char *key, uint32_t *out);

    /** Optional unsigned 32-bit field. Absent key -> disengaged optional, ESP_OK. */
    static esp_err_t get_opt_u32(ryml::ConstNodeRef node, const char *key, std::optional<uint32_t> &out);

    /** Required boolean field ("true"/"false"/"1"/"0"/"yes"/"no"). */
    static esp_err_t get_bool(ryml::ConstNodeRef node, const char *key, bool *out);

    /** Optional boolean field. */
    static esp_err_t get_opt_bool(ryml::ConstNodeRef node, const char *key, std::optional<bool> &out);

    /** Required string field, copied into @p out (NUL terminated). Too-long values error. */
    static esp_err_t get_str(ryml::ConstNodeRef node, const char *key, char *out, size_t out_len);

    /** True when the node carries the given YAML tag (e.g. "!Ram"). */
    static bool has_tag(ryml::ConstNodeRef node, const char *tag);

    // ---------------------------------------------------------------
    // Validating scalar getters (direct node value)
    // ---------------------------------------------------------------

    /** Parse a scalar node value as u32. @p ctx names the node in error logs. */
    static esp_err_t node_u32(ryml::ConstNodeRef val_node, const char *ctx, uint32_t *out);

private:
    char *buf = nullptr;
    ryml::Tree tree = {};
    bool loaded = false;
};

/**
 * Install rapidyaml callbacks backed by PSRAM so ryml internal allocations
 * never touch (and never fragment) internal RAM. Safe to call repeatedly;
 * only the first call takes effect. Must be called before any ryml parsing.
 */
void yaml_install_psram_callbacks();
