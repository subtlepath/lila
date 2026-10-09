#pragma once

#include <Memory.h>

#include "HalFontRemovalReferences.h"
#include "HalSingleFileRemovalCohortParticipant.h"

namespace companion {
// Font buffers are allocated once, only when this serialized session removes a font.
class HalRemovalParticipantRouter final : public ContentRemovalParticipant {
 public:
  HalRemovalParticipantRouter(ContentRemovalParticipant& epub, ContentRemovalJournal& journal,
                              HalMultiPathRemovalPlanStorage& plans, std::span<uint8_t> io,
                              FontRemovalSettings* settings)
      : epub(epub), journal(journal), plans(plans), io(io), settings(settings) {}
  bool verifyPlan(const ContentRemovalRecord& r) override {
    auto* p = select(r);
    return p && p->verifyPlan(r);
  }
  bool quarantine(const ContentRemovalRecord& r) override {
    auto* p = select(r);
    return p && p->quarantine(r);
  }
  bool verifyQuarantined(const ContentRemovalRecord& r) override {
    auto* p = select(r);
    return p && p->verifyQuarantined(r);
  }
  bool publishRemoval(const ContentRemovalRecord& r) override {
    auto* p = select(r);
    return p && p->publishRemoval(r);
  }
  bool verifyPublished(const ContentRemovalRecord& r) override {
    auto* p = select(r);
    return p && p->verifyPublished(r);
  }
  bool retireBackups(const ContentRemovalRecord& r) override {
    auto* p = select(r);
    return p && p->retireBackups(r);
  }
  bool verifyRetired(const ContentRemovalRecord& r) override {
    auto* p = select(r);
    return p && p->verifyRetired(r);
  }

 private:
  struct FontOwner {
    HalFontRemovalReferences references;
    HalSingleFileRemovalCohortParticipant cohort;
    FontOwner(ContentRemovalJournal& journal, HalMultiPathRemovalPlanStorage& plans, std::span<uint8_t> io,
              FontRemovalSettings& settings)
        : references(journal, settings), cohort(journal, plans, references, io) {}
  };
  ContentRemovalParticipant& epub;
  ContentRemovalJournal& journal;
  HalMultiPathRemovalPlanStorage& plans;
  std::span<uint8_t> io;
  FontRemovalSettings* settings;
  std::unique_ptr<FontOwner> font;
  ContentRemovalParticipant* select(const ContentRemovalRecord& r) {
    if (r.request.manifest.kind == ContentKind::Epub) return &epub;
    if (r.request.manifest.kind != ContentKind::Font || !settings) return nullptr;
    if (!font) {
      if (!admitCompanionHeap(sizeof(FontOwner), sizeof(FontOwner))) return nullptr;
      font = makeUniqueNoThrow<FontOwner>(journal, plans, io, *settings);
      if (!font) {
        LOG_ERR("COMPANION", "OOM: font removal cohort owner");
        return nullptr;
      }
    }
    return admitCompanionHeap() ? &font->cohort : nullptr;
  }
};
}  // namespace companion
