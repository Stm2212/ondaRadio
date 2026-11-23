#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef esp_err_t (*radio_nvs_job_fn_t)(void *context);
typedef void (*radio_nvs_completion_fn_t)(esp_err_t result, void *context);

/* One bounded queue and one task own all runtime NVS mutations. */
esp_err_t radio_nvs_worker_init(void);
esp_err_t radio_nvs_worker_submit(radio_nvs_job_fn_t job, void *job_context,
                                  radio_nvs_completion_fn_t completion,
                                  void *completion_context,
                                  TickType_t queue_wait_ticks);
esp_err_t radio_nvs_worker_run_sync(radio_nvs_job_fn_t job, void *context);
