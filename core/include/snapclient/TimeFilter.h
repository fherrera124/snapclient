#pragma once

#include <cstdint>

namespace snapclient {

// Two-state (offset, drift) Kalman filter estimating the client-to-server
// clock offset from round-trip time samples, with adaptive forgetting on
// outlier residuals and a significance gate on the drift term.
class TimeFilter {
 public:
  TimeFilter(double processStdDev, double driftProcessStdDev,
             double forgetFactor, double adaptiveCutoff, uint8_t minSamples,
             double driftSignificanceThreshold);

  // measurementUs/maxErrorUs/timeUs are all in microseconds.
  void insert(int64_t measurementUs, int64_t maxErrorUs, int64_t timeUs);

  int64_t offsetAt(int64_t clientTimeUs) const;
  void reset();
  bool isFull(uint32_t n) const { return count_ >= n; }

 private:
  double processVariance_;
  double driftProcessVariance_;
  double forgetVarianceFactor_;
  double adaptiveForgettingCutoff_;
  double driftSignificanceThresholdSquared_;
  uint8_t minSamplesForForgetting_;

  int64_t lastUpdate_ = 0;
  uint8_t count_ = 0;
  double offset_ = 0.0;
  double drift_ = 0.0;
  double offsetCovariance_ = 0.0;
  double offsetDriftCovariance_ = 0.0;
  double driftCovariance_ = 0.0;
  bool useDrift_ = false;
};

}  // namespace snapclient
