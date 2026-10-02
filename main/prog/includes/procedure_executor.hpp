#pragma once

#include <esp_err.h>
#include <job.pb.h>

#include "target_backend.hpp"

/**
 * Pre/post programming procedure executor.
 *
 * Steps come from the decoded programming job (see job_decoder.hpp) and are
 * executed in place: the executor owns no step storage. Target operations go
 * through target_backend so the same procedures work for every target family.
 */
class procedure_executor
{
public:
    procedure_executor() = delete;

    /** Execute all steps against @p backend; aborts on the first unignored failure. */
    static esp_err_t execute(const si_job_Procedure &procedure, target_backend &backend);

private:
    static esp_err_t exec_one(const si_job_Step &step, target_backend &backend);
    static esp_err_t exec_read32(const si_job_Read32 &op, target_backend &backend);
    static esp_err_t exec_read_mod_write32(const si_job_ReadModWrite32 &op, target_backend &backend);
    static esp_err_t exec_poll32(const si_job_Poll32 &op, target_backend &backend);
    static esp_err_t exec_delay(const si_job_Delay &op);

    static constexpr char TAG[] = "procedure_exec";
};
