#pragma once

#include <cstdint>

#include "core/srs/FsrsDefaults.h"

namespace tinta::core {

// The learner's answer. Values are FSRS ratings and are stored in the journal.
enum class Grade : uint8_t { Again = 1, Hard = 2, Good = 3, Easy = 4 };

// The FSRS memory model, ported from the py-fsrs version named in
// FsrsDefaults.h (FSRS-6, 21 weights) and checked against its output by
// test/host/fsrs_test.cpp. Only the day-based model is ported: learning steps
// are session-relative and live in Review.cpp, and there is no fuzz.
//
// Everything is `float`; the ESP32-C3 has no FPU and a review costs a few
// soft-float pow/exp calls. Constants that depend only on the weights and
// settings are computed once in configure().
class Fsrs {
 public:
  struct Memory {
    float stability;   // days until retrievability falls to 90 %
    float difficulty;  // 1..10
  };

  static constexpr float kMinStability = 0.001f;
  static constexpr float kMinDifficulty = 1.0f;
  static constexpr float kMaxDifficulty = 10.0f;

  explicit Fsrs(float desiredRetention = 0.9f, uint16_t maxInterval = 365, const float* weights = kFsrsDefaultWeights);

  // desiredRetention in (0, 1); maxInterval >= 1 day.
  void configure(float desiredRetention, uint16_t maxInterval);
  float desiredRetention() const { return retention_; }
  uint16_t maxInterval() const { return maxInterval_; }

  // State after the first review of an item.
  Memory initial(Grade grade) const;

  // State after a review `elapsedDays` after the previous one. Zero elapsed
  // days is a same-day review and uses FSRS's short-term formula.
  Memory next(const Memory& memory, uint32_t elapsedDays, Grade grade) const;

  // Probability of recall `elapsedDays` after the last review.
  float retrievability(float stability, uint32_t elapsedDays) const;

  // Days until retrievability falls to the desired retention, rounded as the
  // reference does (half to even), clamped to 1..maxInterval.
  uint16_t interval(float stability) const;
  float intervalUnrounded(float stability) const { return stability * intervalScale_; }

 private:
  float shortTermStability(float stability, Grade grade) const;
  float nextDifficulty(float difficulty, Grade grade) const;

  float w_[kFsrsWeightCount];
  float retention_ = 0.9f;
  uint16_t maxInterval_ = 365;

  float decay_ = 0;              // -w20
  float factor_ = 0;             // 0.9^(1/decay) - 1
  float intervalScale_ = 0;      // (retention^(1/decay) - 1) / factor
  float expW8_ = 0;              // e^w8
  float forgetShortDiv_ = 0;     // e^(w17 * w18)
  float easyDifficulty_ = 0;     // D0(Easy), unclamped: the mean-reversion target
  float shortTermGain_[4] = {};  // e^(w17 * (grade - 3 + w18)) per grade
};

}  // namespace tinta::core
