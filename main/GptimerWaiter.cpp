#include "GptimerWaiter.h"

#include <bell/Logger.h>

namespace snapclient {

namespace {
const char* kLogTag = "GptimerWaiter";
}  // namespace

GptimerWaiter::GptimerWaiter() : taskHandle_(xTaskGetCurrentTaskHandle()) {
  gptimer_config_t timerConfig = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = 1000000,  // 1MHz, 1 tick = 1us
      .intr_priority = 0,
      .flags = {},
  };
  esp_err_t err = gptimer_new_timer(&timerConfig, &timer_);
  if (err != ESP_OK) {
    BELL_LOG(error, kLogTag, "gptimer_new_timer failed: {}",
             static_cast<int>(err));
    timer_ = nullptr;
    return;
  }

  gptimer_event_callbacks_t callbacks = {.on_alarm = &GptimerWaiter::onAlarm};
  err = gptimer_register_event_callbacks(timer_, &callbacks, this);
  if (err != ESP_OK) {
    BELL_LOG(error, kLogTag, "gptimer_register_event_callbacks failed: {}",
             static_cast<int>(err));
    gptimer_del_timer(timer_);
    timer_ = nullptr;
  }
}

GptimerWaiter::~GptimerWaiter() {
  if (timer_ != nullptr) {
    gptimer_del_timer(timer_);
  }
}

void GptimerWaiter::arm(int64_t waitUs) {
  if (waitUs <= 0 || timer_ == nullptr) {
    pendingAlarm_ = false;
    return;
  }

  ESP_ERROR_CHECK(gptimer_enable(timer_));
  ESP_ERROR_CHECK(gptimer_set_raw_count(timer_, 0));
  gptimer_alarm_config_t alarmConfig = {
      .alarm_count = static_cast<uint64_t>(waitUs),
      .reload_count = 0,
      .flags = {.auto_reload_on_alarm = false},
  };
  ESP_ERROR_CHECK(gptimer_set_alarm_action(timer_, &alarmConfig));
  ESP_ERROR_CHECK(gptimer_start(timer_));
  pendingAlarm_ = true;
}

void GptimerWaiter::block() {
  if (!pendingAlarm_) {
    return;
  }
  xTaskNotifyWait(0, 0, nullptr, portMAX_DELAY);
  ESP_ERROR_CHECK(gptimer_stop(timer_));
  ESP_ERROR_CHECK(gptimer_disable(timer_));
  pendingAlarm_ = false;
}

// IRAM_ATTR: the driver may invoke this while flash cache is disabled, so
// it can only call other IRAM-safe functions.
bool IRAM_ATTR GptimerWaiter::onAlarm(gptimer_handle_t /*timer*/,
                                      const gptimer_alarm_event_data_t*
                                          /*edata*/,
                                      void* userCtx) {
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  xTaskNotifyFromISR(static_cast<GptimerWaiter*>(userCtx)->taskHandle_, 0,
                     eNoAction, &higherPriorityTaskWoken);
  return higherPriorityTaskWoken == pdTRUE;
}

}  // namespace snapclient
