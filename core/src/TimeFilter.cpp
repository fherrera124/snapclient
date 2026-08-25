#include "snapclient/TimeFilter.h"

#include <cmath>
#include <limits>

namespace snapclient {

TimeFilter::TimeFilter(double processStdDev, double driftProcessStdDev,
                       double forgetFactor, double adaptiveCutoff,
                       uint8_t minSamples, double driftSignificanceThreshold)
    : processVariance_(processStdDev * processStdDev),
      driftProcessVariance_(driftProcessStdDev * driftProcessStdDev),
      forgetVarianceFactor_(forgetFactor * forgetFactor),
      adaptiveForgettingCutoff_(adaptiveCutoff),
      driftSignificanceThresholdSquared_(driftSignificanceThreshold *
                                         driftSignificanceThreshold),
      minSamplesForForgetting_(minSamples) {
  reset();
}

void TimeFilter::reset() {
  count_ = 0;
  offset_ = 0.0;
  drift_ = 0.0;
  offsetCovariance_ = std::numeric_limits<double>::infinity();
  offsetDriftCovariance_ = 0.0;
  driftCovariance_ = 0.0;
  lastUpdate_ = 0;
  useDrift_ = false;
}

void TimeFilter::insert(int64_t measurementUs, int64_t maxErrorUs,
                        int64_t timeUs) {
  if (timeUs <= lastUpdate_) {
    return;
  }

  const double dt = static_cast<double>(timeUs - lastUpdate_);
  const double dtSquared = dt * dt;
  lastUpdate_ = timeUs;

  const double measurementVariance =
      static_cast<double>(maxErrorUs) * static_cast<double>(maxErrorUs);

  if (count_ == 0) {
    ++count_;
    offset_ = static_cast<double>(measurementUs);
    offsetCovariance_ = measurementVariance;
    drift_ = 0.0;
    return;
  }

  if (count_ == 1) {
    ++count_;
    drift_ = (static_cast<double>(measurementUs) - offset_) / dt;
    offset_ = static_cast<double>(measurementUs);
    driftCovariance_ = (offsetCovariance_ + measurementVariance) / dtSquared;
    offsetCovariance_ = measurementVariance;
    return;
  }

  // Predict.
  double offset = offset_ + drift_ * dt;

  const double driftProcessVariance = dt * driftProcessVariance_;
  double newDriftCovariance = driftCovariance_ + driftProcessVariance;
  double newOffsetDriftCovariance =
      offsetDriftCovariance_ + driftCovariance_ * dt;
  const double offsetProcessVariance = dt * processVariance_;
  double newOffsetCovariance =
      offsetCovariance_ + 2 * offsetDriftCovariance_ * dt +
      driftCovariance_ * dtSquared + offsetProcessVariance;

  // Innovation and adaptive forgetting.
  const double residual = static_cast<double>(measurementUs) - offset;
  const double maxResidualCutoff =
      static_cast<double>(maxErrorUs) * adaptiveForgettingCutoff_;

  if (count_ < minSamplesForForgetting_) {
    ++count_;
  } else if (std::fabs(residual) > maxResidualCutoff) {
    newDriftCovariance *= forgetVarianceFactor_;
    newOffsetDriftCovariance *= forgetVarianceFactor_;
    newOffsetCovariance *= forgetVarianceFactor_;
  }

  // Update.
  const double uncertainty = 1.0 / (newOffsetCovariance + measurementVariance);
  const double offsetGain = newOffsetCovariance * uncertainty;
  const double driftGain = newOffsetDriftCovariance * uncertainty;

  offset_ = offset + offsetGain * residual;
  drift_ += driftGain * residual;

  driftCovariance_ = newDriftCovariance - driftGain * newOffsetDriftCovariance;
  offsetDriftCovariance_ =
      newOffsetDriftCovariance - driftGain * newOffsetCovariance;
  offsetCovariance_ = newOffsetCovariance - offsetGain * newOffsetCovariance;

  const double driftSquared = drift_ * drift_;
  useDrift_ =
      driftSquared > driftSignificanceThresholdSquared_ * driftCovariance_;
}

int64_t TimeFilter::offsetAt(int64_t clientTimeUs) const {
  const double dt = static_cast<double>(clientTimeUs - lastUpdate_);
  const double effectiveDrift = useDrift_ ? drift_ : 0.0;
  return static_cast<int64_t>(std::llround(offset_ + effectiveDrift * dt));
}

}  // namespace snapclient
