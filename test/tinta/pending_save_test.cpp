// modules:
#include "check.h"
#include "core/PendingSave.h"

using tinta::core::PendingSave;

int main() {
  PendingSave save;
  CHECK(!save.pending());
  CHECK(!save.due(0));
  save.mark();
  CHECK(save.due(0));
  save.complete(100, false);
  CHECK(save.pending());
  CHECK(!save.due(1099));
  save.mark();
  CHECK(!save.due(1099));
  CHECK(save.due(1100));
  save.complete(1100, true);
  CHECK(!save.pending());
  save.mark();
  CHECK(!save.due(1101));
  CHECK(save.due(2100));
  save.complete(0xfffffff0u, false);
  CHECK(!save.due(0x000003d7u));
  CHECK(save.due(0x000003d8u));
  save.complete(0x000003d8u, true);
  CHECK(!save.pending());
  PendingSave upgraded(true);
  CHECK(upgraded.pending());
  CHECK(upgraded.due(0));
  return tinta_test::result();
}
