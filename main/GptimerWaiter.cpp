#include "GptimerWaiter.h"

#include <algorithm>

#include <bell/Logger.h>

namespace snapclient {

namespace {
const char* kLogTag = "GptimerWaiter";
constexpr int64_t kAlarmSlackMs = 100;
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

  xTaskNotifyStateClear(nullptr);
  armedUs_ = waitUs;
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
  const int64_t timeoutTicks =
      (armedUs_ / 1000 + kAlarmSlackMs) * configTICK_RATE_HZ / 1000 + 1;
  if (xTaskNotifyWait(0, 0, nullptr,
                      static_cast<TickType_t>(std::min<int64_t>(
                          timeoutTicks, portMAX_DELAY - 1))) != pdTRUE) {
    uint64_t countUs = 0;
    gptimer_get_raw_count(timer_, &countUs);
    BELL_LOG(error, kLogTag, "alarm did not fire: armed {}us, timer at {}us",
             armedUs_, countUs);
  }
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
