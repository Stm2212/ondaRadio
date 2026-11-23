#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RADIO_TEST_OPERATION_ALLOCATION = 0,
    RADIO_TEST_OPERATION_CLOCK,
    RADIO_TEST_OPERATION_NETWORK,
    RADIO_TEST_OPERATION_EVENT,
    RADIO_TEST_OPERATION_NVS_COMMIT,
    RADIO_TEST_OPERATION_COUNT,
} radio_test_operation_t;

#ifdef RADIO_INTEGRATION_TEST_HOOKS

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"

typedef int64_t (*radio_test_clock_source_fn_t)(void *context);

void radio_test_adapter_reset(void);
void radio_test_adapter_fail_on(radio_test_operation_t operation,
                                uint32_t one_based_attempt);
void radio_test_adapter_clear_fault(radio_test_operation_t operation);
uint32_t radio_test_adapter_attempts(radio_test_operation_t operation);
bool radio_test_adapter_should_fail(radio_test_operation_t operation);
void radio_test_adapter_set_clock_source(radio_test_clock_source_fn_t source,
                                         void *context);
void radio_test_adapter_set_clock_failure_sample(int64_t sample_us);
int64_t radio_test_adapter_clock_us(int64_t production_time_us);
void *radio_test_adapter_malloc(size_t size);
void *radio_test_adapter_heap_caps_malloc(size_t size, uint32_t caps);
void *radio_test_adapter_heap_caps_calloc(size_t count, size_t size,
                                          uint32_t caps);
esp_err_t radio_test_adapter_network_gate(void);
BaseType_t radio_test_adapter_queue_send(QueueHandle_t queue,
                                         const void *item,
                                         TickType_t wait_ticks);
BaseType_t radio_test_adapter_queue_overwrite(QueueHandle_t queue,
                                              const void *item);
esp_err_t radio_test_adapter_nvs_commit_gate(void);
esp_err_t radio_test_adapter_nvs_commit(nvs_handle_t handle);

#define RADIO_TEST_FAULT(operation) \
    radio_test_adapter_should_fail((operation))
#define RADIO_TEST_CLOCK_US(production_time_us) \
    radio_test_adapter_clock_us((production_time_us))
#define RADIO_TEST_MALLOC(size) radio_test_adapter_malloc((size))
#define RADIO_TEST_HEAP_CAPS_MALLOC(size, caps) \
    radio_test_adapter_heap_caps_malloc((size), (caps))
#define RADIO_TEST_HEAP_CAPS_CALLOC(count, size, caps) \
    radio_test_adapter_heap_caps_calloc((count), (size), (caps))
#define RADIO_TEST_NETWORK_GATE() radio_test_adapter_network_gate()
#define RADIO_TEST_QUEUE_SEND(queue, item, wait_ticks) \
    radio_test_adapter_queue_send((queue), (item), (wait_ticks))
#define RADIO_TEST_QUEUE_OVERWRITE(queue, item) \
    radio_test_adapter_queue_overwrite((queue), (item))
#define RADIO_TEST_NVS_COMMIT(handle) radio_test_adapter_nvs_commit((handle))

#else

/* Product builds compile these seams directly to their production operations. */
#define RADIO_TEST_FAULT(operation) (false)
#define RADIO_TEST_CLOCK_US(production_time_us) (production_time_us)
#define RADIO_TEST_MALLOC(size) malloc((size))
#define RADIO_TEST_HEAP_CAPS_MALLOC(size, caps) \
    heap_caps_malloc((size), (caps))
#define RADIO_TEST_HEAP_CAPS_CALLOC(count, size, caps) \
    heap_caps_calloc((count), (size), (caps))
#define RADIO_TEST_NETWORK_GATE() ESP_OK
#define RADIO_TEST_QUEUE_SEND(queue, item, wait_ticks) \
    xQueueSend((queue), (item), (wait_ticks))
#define RADIO_TEST_QUEUE_OVERWRITE(queue, item) \
    xQueueOverwrite((queue), (item))
#define RADIO_TEST_NVS_COMMIT(handle) nvs_commit((handle))

#endif
