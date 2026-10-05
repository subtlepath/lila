#include "core/srs/Fsrs.h"

#include <cmath>

namespace tinta::core {

namespace {

float clampDifficulty(float d) {
  if (d < Fsrs::kMinDifficulty) return Fsrs::kMinDifficulty;
  if (d > Fsrs::kMaxDifficulty) return Fsrs::kMaxDifficulty;
  return d;
}

float clampStability(float s) { return s < Fsrs::kMinStability ? Fsrs::kMinStability : s; }

float gradeValue(Grade grade) { return static_cast<float>(static_cast<uint8_t>(grade)); }

// Python's round(): half to even. Ties are rare in float but cost nothing.
float roundHalfEven(float x) {
  const float lower = std::floor(x);
  const float frac = x - lower;
  if (frac > 0.5f) return lower + 1.0f;
  if (frac < 0.5f) return lower;
  return std::fmod(lower, 2.0f) == 0.0f ? lower : lower + 1.0f;
}

}  // namespace

Fsrs::Fsrs(float desiredRetention, uint16_t maxInterval, const float* weights) {
  for (int i = 0; i < kFsrsWeightCount; ++i) w_[i] = weights[i];
  decay_ = -w_[20];
  factor_ = std::pow(0.9f, 1.0f / decay_) - 1.0f;
  expW8_ = std::exp(w_[8]);
  forgetShortDiv_ = std::exp(w_[17] * w_[18]);
  easyDifficulty_ = w_[4] - std::exp(w_[5] * 3.0f) + 1.0f;
  for (int g = 0; g < 4; ++g) {
    shortTermGain_[g] = std::exp(w_[17] * (static_cast<float>(g + 1) - 3.0f + w_[18]));
  }
  configure(desiredRetention, maxInterval);
}

void Fsrs::configure(float desiredRetention, uint16_t maxInterval) {
  retention_ = desiredRetention;
  maxInterval_ = maxInterval < 1 ? 1 : maxInterval;
  intervalScale_ = (std::pow(retention_, 1.0f / decay_) - 1.0f) / factor_;
}

Fsrs::Memory Fsrs::initial(Grade grade) const {
  const int g = static_cast<int>(grade);
  Memory m;
  m.stability = clampStability(w_[g - 1]);
  m.difficulty = clampDifficulty(w_[4] - std::exp(w_[5] * static_cast<float>(g - 1)) + 1.0f);
  return m;
}

float Fsrs::retrievability(float stability, uint32_t elapsedDays) const {
  if (stability <= 0.0f) return 0.0f;
  const float t = static_cast<float>(elapsedDays);
  return std::pow(1.0f + factor_ * t / stability, decay_);
}

uint16_t Fsrs::interval(float stability) const {
  const float raw = roundHalfEven(intervalUnrounded(stability));
  if (raw >= static_cast<float>(maxInterval_)) return maxInterval_;
  if (raw < 1.0f) return 1;
  return static_cast<uint16_t>(raw);
}

float Fsrs::shortTermStability(float stability, Grade grade) const {
  float gain = shortTermGain_[static_cast<int>(grade) - 1] * std::pow(stability, -w_[19]);
  if (grade != Grade::Again && gain < 1.0f) gain = 1.0f;
  return clampStability(stability * gain);
}

float Fsrs::nextDifficulty(float difficulty, Grade grade) const {
  const float delta = -(w_[6] * (gradeValue(grade) - 3.0f));
  const float damped = difficulty + (10.0f - difficulty) * delta / 9.0f;
  return clampDifficulty(w_[7] * easyDifficulty_ + (1.0f - w_[7]) * damped);
}

Fsrs::Memory Fsrs::next(const Memory& memory, uint32_t elapsedDays, Grade grade) const {
  const float s = memory.stability;
  const float d = memory.difficulty;
  Memory out;
  if (elapsedDays == 0) {
    out.stability = shortTermStability(s, grade);
  } else {
    const float r = retrievability(s, elapsedDays);
    float next;
    if (grade == Grade::Again) {
      // (s + 1)^w13 - 1 cancels badly for small s in float; expm1/log1p keep
      // the short stabilities a run of lapses produces accurate.
      const float longTerm =
          w_[11] * std::pow(d, -w_[12]) * std::expm1(w_[13] * std::log1p(s)) * std::exp((1.0f - r) * w_[14]);
      const float shortTerm = s / forgetShortDiv_;
      next = longTerm < shortTerm ? longTerm : shortTerm;
    } else {
      const float hardPenalty = grade == Grade::Hard ? w_[15] : 1.0f;
      const float easyBonus = grade == Grade::Easy ? w_[16] : 1.0f;
      next = s * (1.0f + expW8_ * (11.0f - d) * std::pow(s, -w_[9]) * (std::exp((1.0f - r) * w_[10]) - 1.0f) *
                             hardPenalty * easyBonus);
    }
    out.stability = clampStability(next);
  }
  out.difficulty = nextDifficulty(d, grade);
  return out;
}

}  // namespace tinta::core
