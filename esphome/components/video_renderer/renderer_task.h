#pragma once
#ifdef USE_ESP_IDF
#include "esphome/core/log.h"
#include <cstdlib>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
namespace esphome::video_renderer {
// The persistent worker exits upstream codec calls before publishing done.
inline bool start_managed_pinned_task(TaskFunction_t fn, const char *name,
    uint32_t stack_bytes, void *param, UBaseType_t priority, BaseType_t core,
    bool psram_stack, const char *tag, TaskHandle_t *handle, StaticTask_t *,
    StackType_t **stack, bool *with_caps) {
  *handle = nullptr;
  *stack = nullptr;
  *with_caps = psram_stack;
  const BaseType_t result = psram_stack
      ? xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, param, priority,
                                      handle, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
      : xTaskCreatePinnedToCore(fn, name, stack_bytes, param, priority, handle, core);
  if (result != pdPASS || *handle == nullptr) {
    *handle = nullptr;
    ESP_LOGE(tag, "Failed to create %s task", name);
    return false;
  }
  return true;
}
[[noreturn]] inline void finish_managed_pinned_task(SemaphoreHandle_t done) {
  xSemaphoreGive(done);
  vTaskSuspend(nullptr);
  abort();
}
inline void cleanup_managed_pinned_task(TaskHandle_t *handle, StackType_t **,
    uint32_t, bool with_caps) {
  if (handle == nullptr || *handle == nullptr) return;
  if (with_caps) vTaskDeleteWithCaps(*handle);
  else vTaskDelete(*handle);
  *handle = nullptr;
}
}  // namespace esphome::video_renderer
#endif
