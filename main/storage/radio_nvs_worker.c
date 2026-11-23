#include "radio_nvs_worker.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radio_task_supervisor.h"
#include "radio_test_adapters.h"

#define NVS_WORK_QUEUE_LENGTH 8U
#define NVS_WORKER_STACK_SIZE 4096U
#define NVS_WORKER_PRIORITY 3U
#define NVS_SLOW_JOB_WARN_US 500000LL

static const char *TAG = "radio_nvs";

typedef struct {
    radio_nvs_job_fn_t job;
    void *job_context;
    radio_nvs_completion_fn_t completion;
    void *completion_context;
} nvs_work_item_t;

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
} sync_completion_t;

static QueueHandle_t s_work_queue;
static TaskHandle_t s_worker_task;
static UBaseType_t s_stack_low_water_free_bytes = (UBaseType_t)-1;

static void worker_task(void *arg)
{
    (void)arg;
    radio_task_supervisor_register(RADIO_SUPERVISED_TASK_NVS);
    nvs_work_item_t item;
    for (;;) {
        if (xQueueReceive(s_work_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
            radio_task_supervisor_progress(RADIO_SUPERVISED_TASK_NVS);
            continue;
        }
        const int64_t job_start_us = esp_timer_get_time();
        const esp_err_t result = item.job(item.job_context);
        const int64_t job_elapsed_us = esp_timer_get_time() - job_start_us;
        const UBaseType_t stack_free_bytes = uxTaskGetStackHighWaterMark(NULL);
        if (stack_free_bytes < s_stack_low_water_free_bytes) {
            s_stack_low_water_free_bytes = stack_free_bytes;
            ESP_LOGI(TAG, "NVS worker stack low-water: %u bytes",
                     (unsigned)stack_free_bytes);
        }
        if (job_elapsed_us >= NVS_SLOW_JOB_WARN_US) {
            ESP_LOGW(TAG, "NVS job took %lld us",
                     (long long)job_elapsed_us);
        }
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "NVS job failed after %lld us: %s",
                     (long long)job_elapsed_us, esp_err_to_name(result));
        } else {
            ESP_LOGD(TAG, "NVS job timing elapsed_us=%lld",
                     (long long)job_elapsed_us);
        }
        if (item.completion != NULL) {
            item.completion(result, item.completion_context);
        }
        radio_task_supervisor_progress(RADIO_SUPERVISED_TASK_NVS);
    }
}

esp_err_t radio_nvs_worker_init(void)
{
    if (s_worker_task != NULL) return ESP_OK;
    s_work_queue = xQueueCreate(NVS_WORK_QUEUE_LENGTH, sizeof(nvs_work_item_t));
    if (s_work_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker_task, "radio_nvs", NVS_WORKER_STACK_SIZE, NULL,
                    NVS_WORKER_PRIORITY, &s_worker_task) != pdPASS) {
        vQueueDelete(s_work_queue);
        s_work_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t radio_nvs_worker_submit(radio_nvs_job_fn_t job, void *job_context,
                                  radio_nvs_completion_fn_t completion,
                                  void *completion_context,
                                  TickType_t queue_wait_ticks)
{
    if (job == NULL) return ESP_ERR_INVALID_ARG;
    if (s_work_queue == NULL || s_worker_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const nvs_work_item_t item = {
        .job = job,
        .job_context = job_context,
        .completion = completion,
        .completion_context = completion_context,
    };
    return RADIO_TEST_QUEUE_SEND(s_work_queue, &item, queue_wait_ticks) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void sync_completion_signal(esp_err_t result, void *context)
{
    sync_completion_t *completion = context;
    completion->result = result;
    xSemaphoreGive(completion->done);
}

esp_err_t radio_nvs_worker_run_sync(radio_nvs_job_fn_t job, void *context)
{
    if (job == NULL) return ESP_ERR_INVALID_ARG;
    if (s_worker_task == NULL) return ESP_ERR_INVALID_STATE;
    if (xTaskGetCurrentTaskHandle() == s_worker_task) return ESP_ERR_INVALID_STATE;

    sync_completion_t completion = {
        .done = xSemaphoreCreateBinary(),
        .result = ESP_FAIL,
    };
    if (completion.done == NULL) return ESP_ERR_NO_MEM;
    const esp_err_t submit_err = radio_nvs_worker_submit(
        job, context, sync_completion_signal, &completion,
        pdMS_TO_TICKS(5000));
    int64_t wait_elapsed_us = 0;
    if (submit_err == ESP_OK) {
        const int64_t wait_start_us = esp_timer_get_time();
        (void)xSemaphoreTake(completion.done, portMAX_DELAY);
        wait_elapsed_us = esp_timer_get_time() - wait_start_us;
        if (wait_elapsed_us >= NVS_SLOW_JOB_WARN_US) {
            ESP_LOGW(TAG, "Synchronous NVS caller waited %lld us",
                     (long long)wait_elapsed_us);
        }
    }
    const esp_err_t result = submit_err == ESP_OK
                                 ? completion.result : submit_err;
    if (submit_err == ESP_OK) {
        ESP_LOGD(TAG, "NVS sync timing wait_us=%lld result=%s",
                 (long long)wait_elapsed_us, esp_err_to_name(result));
    }
    vSemaphoreDelete(completion.done);
    return result;
}
