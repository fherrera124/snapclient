#pragma once

#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "snapclient/PrecisionWaiter.h"

namespace snapclient {

// Must be constructed on the task that will call waitUs() - captures that
// task's handle for onAlarm() to notify.
class GptimerWaiter : public PrecisionWaiter {
 public:
  GptimerWaiter();
  ~GptimerWaiter();

  GptimerWaiter(const GptimerWaiter&) = delete;
  GptimerWaiter& operator=(const GptimerWaiter&) = delete;

  // No-op (returns immediately) if construction failed to obtain a timer.
  void waitUs(int64_t waitUs) override;

 private:
  static bool IRAM_ATTR onAlarm(gptimer_handle_t timer,
                                const gptimer_alarm_event_data_t* edata,
                                void* userCtx);

  gptimer_handle_t timer_ = nullptr;
  const TaskHandle_t taskHandle_;
};

}  // namespace snapclient
