#pragma once

#include <cstdint>

#include "core/pack/Pack.h"

namespace tinta::core::search {

// Dictionary search (PLAN.md 8.5, M6): binary searches over the pack's key
// tables in flash, folded the same way as the keys (accents and case do not
// matter), no index in RAM. One query gives, without repeats and in this
// order:
//
//   1. headwords spelled exactly so            LKEY
//   2. headwords with an inflected form so      FORM  ("fui": ir, ser)
//   3. headwords that start so                  LKEY prefix
//   4. headwords with an English sense so       EKEY prefix
//
// up to kMaxResults; more() says when the list was cut.

enum class Match : uint8_t { Headword, Form, Prefix, English };

struct Result {
  uint16_t lemma = pack::kNone16;
  Match how = Match::Headword;
  uint32_t form = 0;  // Form: the inflected form as spelled (a string offset)
};

class Search {
 public:
  static constexpr uint8_t kMaxResults = 24;

  // An empty query (or one of separators only) finds nothing.
  uint8_t run(const pack::Pack& pack, const char* query);

  uint8_t count() const { return count_; }
  const Result& at(uint8_t i) const { return results_[i < count_ ? i : 0]; }
  bool more() const { return more_; }

 private:
  bool add(uint16_t lemma, Match how, uint32_t form = 0);

  Result results_[kMaxResults];
  uint8_t count_ = 0;
  bool more_ = false;
};

// Key devices: the letters that can follow `prefix` among the headwords
// (english: among the English keys). Bit i is letter 'a' + i, bit 26 a space
// (multi-word headwords: "a sus órdenes").
inline constexpr uint8_t kSpaceBit = 26;
uint32_t nextLetters(const pack::Pack& pack, const char* prefix, bool english);

}  // namespace tinta::core::search
