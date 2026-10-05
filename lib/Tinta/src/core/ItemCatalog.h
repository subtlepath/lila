#pragma once

#include <cstdint>

namespace tinta::core {

// What an item asks of the learner. The exercise format shown for an item is
// chosen at review time from the formats valid for its kind (PLAN.md 4.1, 4.3).
enum class ItemKind : uint8_t {
  VocabRecognise = 0,  // Spanish -> meaning
  VocabProduce = 1,    // meaning -> Spanish
  Cloze = 2,           // word in a sentence
  Conjugation = 3,     // verb form for a person and tense
  Gender = 4,          // el / la
  Phrase = 5,          // phrasebook entry
  WordOrder = 6,       // rebuild a sentence
};

// The schedulable items of the course, as the scheduler needs to see them.
// Items are addressed by a dense index (0..itemCount-1) that is only valid for
// one pack build; progress is keyed by the stable uid (PLAN.md section 7.6).
// The content pack implements this; host tests supply a fake.
class ItemCatalog {
 public:
  virtual ~ItemCatalog() = default;

  virtual uint32_t itemCount() const = 0;
  virtual uint32_t uidAt(uint32_t index) const = 0;
  // -1 when the uid is not in this pack (a retired item).
  virtual int32_t indexOfUid(uint32_t uid) const = 0;
  virtual ItemKind kindAt(uint32_t index) const = 0;
  virtual uint16_t lessonAt(uint32_t index) const = 0;
  // The item that must be learnt before this one is introduced (a word's
  // recognise item gates its produce item), or -1.
  virtual int32_t prerequisiteOf(uint32_t index) const = 0;
};

}  // namespace tinta::core
