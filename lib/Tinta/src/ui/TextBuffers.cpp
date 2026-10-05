#include "ui/TextBuffers.h"

#include <new>

namespace tinta::ui {
namespace {

struct Buffers {
  core::text::Span spans[kSharedSpanCap];
  core::text::Run runs[kSharedRunCap];
};

Buffers* gBuffers = nullptr;

}  // namespace

bool openTextBuffers() {
  if (!gBuffers) gBuffers = new (std::nothrow) Buffers;
  return gBuffers != nullptr;
}

void closeTextBuffers() {
  delete gBuffers;
  gBuffers = nullptr;
}

core::text::Span* sharedSpans() { return gBuffers->spans; }
core::text::Run* sharedRuns() { return gBuffers->runs; }

}  // namespace tinta::ui
