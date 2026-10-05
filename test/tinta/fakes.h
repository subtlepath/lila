#pragma once

// Host fakes for the learning engine: a settable Clock, an in-memory
// StateStore with fault injection, and a configurable ItemCatalog. Test code
// only; heap and std containers are fine here.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "core/ItemCatalog.h"
#include "core/StateStore.h"

namespace tinta_test {

using tinta::core::DayNumber;
using tinta::core::ItemKind;

class FakeClock : public tinta::core::Clock {
 public:
  explicit FakeClock(DayNumber day = 1000, bool timeOfDay = true) : day_(day), timeOfDay_(timeOfDay) {}

  DayNumber today() const override { return day_; }
  bool hasTimeOfDay() const override { return timeOfDay_; }
  uint32_t nowSeconds() const override { return static_cast<uint32_t>(day_) * 86400u + seconds_; }

  void setDay(DayNumber day) {
    day_ = day;
    seconds_ = 9 * 3600;
  }
  void nextDay(int n = 1) { setDay(static_cast<DayNumber>(day_ + n)); }
  void tick(uint32_t s) { seconds_ += s; }

 private:
  DayNumber day_;
  bool timeOfDay_;
  uint32_t seconds_ = 9 * 3600;
};

// Files in a map. Every mutating call (write, append, replace, remove) is
// numbered from 0. Faults:
//   failFrom(n)      calls n, n+1, ... fail with no effect, and reads fail
//                    too (the card was pulled);
//   cutAt(n, tear)   power is cut during call n: it leaves `tear` behind and
//                    every later call fails until powerOn();
//   dropAtSize(n)    the card stops answering at the n-th size() call (from
//   dropAtRead(n)    0), or read() call: that call fails, available() turns
//                    false as the platform store's does, and every later call
//                    fails until powerOn(). Mutating calls attempted after
//                    that are counted in attemptsWhileGone.
// replace() is atomic as the StateStore contract requires: a cut leaves the
// old file and a stray temporary.
class MemStore : public tinta::core::StateStore {
 public:
  enum class Tear {
    Nothing,  // the cut call had no effect
    Prefix,   // the first half of its bytes landed
    Zeros,    // the file grew or was overwritten with zeros, not the data
    Garbage,  // the range holds 0xA5 bytes
  };

  std::map<std::string, std::vector<uint8_t>> files;
  bool present = true;  // what available() answers while the card answers
  int calls = 0;        // mutating calls attempted so far
  int sizeCalls = 0;
  int readCalls = 0;
  int attemptsWhileGone = 0;

  void failFrom(int n) { failFrom_ = n; }
  void cutAt(int n, Tear tear) {
    cutAt_ = n;
    tear_ = tear;
  }
  void dropAtSize(int n) { dropAtSize_ = n; }
  void dropAtRead(int n) { dropAtRead_ = n; }
  void powerOn() {
    dead_ = false;
    gone_ = false;
    cutAt_ = -1;
    failFrom_ = -1;
    dropAtSize_ = -1;
    dropAtRead_ = -1;
  }
  bool dead() const { return dead_; }
  bool gone() const { return gone_; }

  bool available() const override { return present && !gone_; }

  int32_t size(const char* name) override {
    if (sizeCalls++ == dropAtSize_) gone_ = true;
    if (blocked()) return -1;
    auto it = files.find(name);
    return it == files.end() ? -1 : static_cast<int32_t>(it->second.size());
  }

  int32_t read(const char* name, uint32_t offset, void* buffer, uint32_t len) override {
    if (readCalls++ == dropAtRead_) gone_ = true;
    if (blocked()) return -1;
    auto it = files.find(name);
    if (it == files.end()) return -1;
    const std::vector<uint8_t>& f = it->second;
    if (offset >= f.size()) return 0;
    const uint32_t n = std::min<uint32_t>(len, static_cast<uint32_t>(f.size() - offset));
    std::memcpy(buffer, f.data() + offset, n);
    return static_cast<int32_t>(n);
  }

  bool write(const char* name, uint32_t offset, const void* data, uint32_t len) override {
    Fault fault = begin();
    if (fault == Fault::Fail) return false;
    std::vector<uint8_t>& f = files[name];
    if (fault == Fault::Cut) {
      tornWrite(f, offset, data, len);
      return false;
    }
    if (f.size() < offset + len) f.resize(offset + len, 0);
    std::memcpy(f.data() + offset, data, len);
    return true;
  }

  bool append(const char* name, const void* data, uint32_t len) override {
    Fault fault = begin();
    if (fault == Fault::Fail) return false;
    std::vector<uint8_t>& f = files[name];
    if (fault == Fault::Cut) {
      tornWrite(f, static_cast<uint32_t>(f.size()), data, len);
      return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    f.insert(f.end(), p, p + len);
    return true;
  }

  bool replace(const char* name, const void* data, uint32_t len) override {
    Fault fault = begin();
    if (fault == Fault::Fail) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (fault == Fault::Cut) {
      std::vector<uint8_t>& tmp = files[std::string(name) + ".tmp"];
      tmp.clear();
      tornWrite(tmp, 0, data, len);
      return false;
    }
    files[name] = std::vector<uint8_t>(p, p + len);
    return true;
  }

  bool remove(const char* name) override {
    Fault fault = begin();
    if (fault != Fault::None) return false;
    return files.erase(name) > 0;
  }

 private:
  enum class Fault { None, Fail, Cut };

  bool blocked() const { return dead_ || gone_ || (failFrom_ >= 0 && calls >= failFrom_); }

  Fault begin() {
    const int n = calls++;
    if (gone_) {
      ++attemptsWhileGone;
      return Fault::Fail;
    }
    if (dead_) return Fault::Fail;
    if (failFrom_ >= 0 && n >= failFrom_) return Fault::Fail;
    if (n == cutAt_) {
      dead_ = true;
      return Fault::Cut;
    }
    return Fault::None;
  }

  void tornWrite(std::vector<uint8_t>& f, uint32_t offset, const void* data, uint32_t len) {
    if (tear_ == Tear::Nothing) return;
    const uint32_t n = tear_ == Tear::Prefix ? len / 2 : len;
    if (f.size() < offset + n) f.resize(offset + n, 0);
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t b = static_cast<const uint8_t*>(data)[i];
      if (tear_ == Tear::Zeros) b = 0;
      if (tear_ == Tear::Garbage) b = 0xA5;
      f[offset + i] = b;
    }
  }

  int failFrom_ = -1;
  int cutAt_ = -1;
  int dropAtSize_ = -1;
  int dropAtRead_ = -1;
  bool gone_ = false;
  Tear tear_ = Tear::Nothing;
  bool dead_ = false;
};

// A catalog defined by a list of items. Prerequisites are held by uid, so a
// reordered or trimmed copy stays consistent.
class FakeCatalog : public tinta::core::ItemCatalog {
 public:
  struct Item {
    uint32_t uid;
    ItemKind kind;
    uint16_t lesson;
    uint32_t prerequisiteUid;  // 0 = none
  };

  FakeCatalog() = default;
  explicit FakeCatalog(std::vector<Item> items) : items_(std::move(items)) { reindex(); }

  // `lemmas` words, each a recognise item followed by a produce item gated by
  // it, `lemmasPerLesson` words per lesson starting at lesson 1. Uids start at
  // `firstUid` and step by one.
  static FakeCatalog vocab(int lemmas, int lemmasPerLesson, uint32_t firstUid = 1000) {
    std::vector<Item> items;
    for (int i = 0; i < lemmas; ++i) {
      const uint16_t lesson = static_cast<uint16_t>(1 + i / lemmasPerLesson);
      const uint32_t rec = firstUid + 2u * static_cast<uint32_t>(i);
      items.push_back({rec, ItemKind::VocabRecognise, lesson, 0});
      items.push_back({rec + 1, ItemKind::VocabProduce, lesson, rec});
    }
    return FakeCatalog(std::move(items));
  }

  const std::vector<Item>& items() const { return items_; }

  // The same items in a shuffled order (a content update that moves items).
  FakeCatalog shuffled(uint32_t seed) const {
    std::vector<Item> items = items_;
    std::mt19937 rng(seed);
    std::shuffle(items.begin(), items.end(), rng);
    return FakeCatalog(std::move(items));
  }

  // A copy without the given uids (retired) and with `added` appended.
  FakeCatalog edited(const std::vector<uint32_t>& retire, const std::vector<Item>& added) const {
    std::vector<Item> items;
    for (const Item& it : items_) {
      if (std::find(retire.begin(), retire.end(), it.uid) == retire.end()) items.push_back(it);
    }
    items.insert(items.end(), added.begin(), added.end());
    return FakeCatalog(std::move(items));
  }

  uint32_t itemCount() const override { return static_cast<uint32_t>(items_.size()); }
  uint32_t uidAt(uint32_t index) const override { return items_[index].uid; }
  int32_t indexOfUid(uint32_t uid) const override {
    auto it = byUid_.find(uid);
    return it == byUid_.end() ? -1 : static_cast<int32_t>(it->second);
  }
  ItemKind kindAt(uint32_t index) const override { return items_[index].kind; }
  uint16_t lessonAt(uint32_t index) const override { return items_[index].lesson; }
  int32_t prerequisiteOf(uint32_t index) const override {
    const uint32_t p = items_[index].prerequisiteUid;
    return p == 0 ? -1 : indexOfUid(p);
  }

 private:
  void reindex() {
    byUid_.clear();
    for (size_t i = 0; i < items_.size(); ++i) byUid_[items_[i].uid] = static_cast<uint32_t>(i);
  }

  std::vector<Item> items_;
  std::map<uint32_t, uint32_t> byUid_;
};

}  // namespace tinta_test
