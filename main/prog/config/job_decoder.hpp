#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>
#include <job.pb.h>

#include "target_config.hpp"

/**
 * Programming jobs compiled on the host by `sidp-agent compile` from
 * target.yaml and the optional pre/post procedure YAML files. The schema is
 * sidp_client/proto/job.proto; the firmware no longer parses YAML.
 */
namespace si::config
{
    /**
     * Decode and validate the job file at @p path into @p job.
     *
     * @p job is large (several KB); the caller allocates it on the heap.
     * For Cortex-M jobs @p algo_bin_out receives a PSRAM buffer holding the
     * flash algorithm, owned by the caller and freed with heap_caps_free().
     * On failure nothing is left allocated and @p job must not be used.
     */
    esp_err_t decode_job(const char *path, si_job_Job &job, uint8_t **algo_bin_out, size_t *algo_bin_len_out);

    /**
     * Publish a validated job into @p cfg without stack copies of the config.
     * Every field of the selected family is rewritten, the other family's
     * fields are cleared and the generation is bumped.
     */
    void fill_target_config(const si_job_Job &job, const uint8_t *algo_bin, size_t algo_bin_len, target_config &cfg);
} // namespace si::config
