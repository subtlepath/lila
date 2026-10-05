#include "ui/views/SpanishKeyboard.h"

#include <string.h>

#include "core/lang/Utf8.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"

namespace tinta::ui {
namespace {

using freeink::ui::KeyboardKey;
using freeink::ui::KeyKind;
using freeink::ui::Rect;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

#define TINTA_KEY(label, value) \
  KeyboardKey { label, label, KeyKind::Normal, freeink::ui::StateNormal, value, 1, true, nullptr }

const KeyboardKey kAccents[] = {TINTA_KEY("á", 225), TINTA_KEY("é", 233), TINTA_KEY("í", 237), TINTA_KEY("ó", 243),
                                TINTA_KEY("ú", 250), TINTA_KEY("ü", 252), TINTA_KEY("ñ", 241)};
const KeyboardKey kRow1[] = {TINTA_KEY("q", 'q'), TINTA_KEY("w", 'w'), TINTA_KEY("e", 'e'), TINTA_KEY("r", 'r'),
                             TINTA_KEY("t", 't'), TINTA_KEY("y", 'y'), TINTA_KEY("u", 'u'), TINTA_KEY("i", 'i'),
                             TINTA_KEY("o", 'o'), TINTA_KEY("p", 'p')};
const KeyboardKey kRow2[] = {TINTA_KEY("a", 'a'), TINTA_KEY("s", 's'), TINTA_KEY("d", 'd'),
                             TINTA_KEY("f", 'f'), TINTA_KEY("g", 'g'), TINTA_KEY("h", 'h'),
                             TINTA_KEY("j", 'j'), TINTA_KEY("k", 'k'), TINTA_KEY("l", 'l')};
#define TINTA_DELETE(label)                                                                                        \
  KeyboardKey {                                                                                                    \
    label, nullptr, KeyKind::Delete, freeink::ui::StateNormal, freeink::ui::QWERTY_KEY_BACKSPACE, 2, true, nullptr \
  }
#define TINTA_SPACE(label) \
  KeyboardKey { label, " ", KeyKind::Space, freeink::ui::StateNormal, freeink::ui::QWERTY_KEY_SPACE, 6, true, nullptr }

const KeyboardKey kRow3[] = {TINTA_KEY("z", 'z'), TINTA_KEY("x", 'x'), TINTA_KEY("c", 'c'), TINTA_KEY("v", 'v'),
                             TINTA_KEY("b", 'b'), TINTA_KEY("n", 'n'), TINTA_KEY("m", 'm'), TINTA_DELETE("del")};
const KeyboardKey kRow4[] = {TINTA_SPACE("space"), KeyboardKey{"OK", nullptr, KeyKind::Ok, freeink::ui::StateNormal,
                                                               freeink::ui::QWERTY_KEY_ENTER, 3, true, nullptr}};
const freeink::ui::KeyboardRow kRows[] = {
    {kAccents, 7, 1}, {kRow1, 10, 0}, {kRow2, 9, 0}, {kRow3, 8, 0}, {kRow4, 2, 0}};
const freeink::ui::KeyboardLayout kLayout{kRows, 5};

// The same keys with the Spanish interface's names for delete and space. The
// English layout stays the one keys are traced and looked up by.
const KeyboardKey kRow3Es[] = {TINTA_KEY("z", 'z'), TINTA_KEY("x", 'x'), TINTA_KEY("c", 'c'), TINTA_KEY("v", 'v'),
                               TINTA_KEY("b", 'b'), TINTA_KEY("n", 'n'), TINTA_KEY("m", 'm'), TINTA_DELETE("borrar")};
const KeyboardKey kRow4Es[] = {TINTA_SPACE("espacio"), KeyboardKey{"OK", nullptr, KeyKind::Ok, freeink::ui::StateNormal,
                                                                   freeink::ui::QWERTY_KEY_ENTER, 3, true, nullptr}};
const freeink::ui::KeyboardRow kRowsEs[] = {
    {kAccents, 7, 1}, {kRow1, 10, 0}, {kRow2, 9, 0}, {kRow3Es, 8, 0}, {kRow4Es, 2, 0}};
const freeink::ui::KeyboardLayout kLayoutEs{kRowsEs, 5};

#undef TINTA_DELETE
#undef TINTA_SPACE

#undef TINTA_KEY

const freeink::ui::Insets kPadding{4, 2, 4, 2};
constexpr int16_t kGap = 4;
constexpr int16_t kRowGap = 6;
constexpr int16_t kRowH = 56;
constexpr int16_t kRowCount = 5;

// The centre of every key, as keyboard() lays them out.
void traceKeys(const Rect area) {
  const Rect rect = area.inset(kPadding);
  const int16_t rowH = i16((rect.height - kRowGap * (kLayout.rowCount - 1)) / kLayout.rowCount);
  for (uint8_t r = 0; r < kLayout.rowCount; ++r) {
    const freeink::ui::KeyboardRow& row = kLayout.rows[r];
    uint16_t units = static_cast<uint16_t>(row.insetUnits * 2);
    for (uint8_t c = 0; c < row.count; ++c) units = static_cast<uint16_t>(units + row.keys[c].widthUnits);
    const int16_t unitW = i16((rect.width - kGap * (row.count - 1)) / units);
    const int16_t y = i16(rect.y + r * (rowH + kRowGap));
    int16_t x = i16(rect.x + row.insetUnits * unitW);
    const int16_t right = i16(rect.right() - row.insetUnits * unitW);
    for (uint8_t c = 0; c < row.count; ++c) {
      const int16_t w = c == row.count - 1 ? i16(right - x) : i16(unitW * row.keys[c].widthUnits);
      platform::log("target key/%s %d %d", row.keys[c].label, x + w / 2, y + rowH / 2);
      x = i16(x + w + kGap);
    }
  }
}

}  // namespace

int16_t SpanishKeyboard::height() { return i16(kRowCount * kRowH + (kRowCount - 1) * kRowGap + 4); }

void SpanishKeyboard::draw(app::UiScreen& screen, const Rect rect, const char* okLabel) {
  freeink::ui::KeyboardProps props;
  props.layout = language() == core::UiLanguage::Spanish ? &kLayoutEs : &kLayout;
  props.keyAction = first_;
  props.deleteAction = static_cast<app::ActionId>(first_ + 1);
  props.okAction = static_cast<app::ActionId>(first_ + 2);
  props.okLabel = okLabel;
  props.labelText.font = kSlotBody;
  props.controlText.font = kSlotBodyBold;
  props.padding = kPadding;
  props.gap = kGap;
  props.rowGap = kRowGap;
  props.minTouchSize = 40;
  freeink::ui::keyboard(screen.frame(), rect, props);
  if (platform::kSimulator && trace_) traceKeys(rect);
  trace_ = false;
}

SpanishKeyboard::Edit SpanishKeyboard::apply(const app::ActionEvent& event, char* text, const size_t cap) const {
  if (event.action == first_) {
    const freeink::ui::KeyboardActivation a = freeink::ui::keyboardActivationFor(kLayout, event.value, event.longPress);
    if (a.kind != freeink::ui::KeyboardActivationKind::Text || !a.text) return Edit::None;
    const size_t used = strlen(text);
    const size_t n = strlen(a.text);
    if (used + n + 1 > cap) return Edit::None;
    memcpy(text + used, a.text, n + 1);
    if (platform::kSimulator) platform::log("input %s", text);
    return Edit::Changed;
  }
  if (event.action == first_ + 1) {
    size_t used = strlen(text);
    while (used > 0) {
      --used;
      if (!core::utf8::isContinuation(static_cast<uint8_t>(text[used]))) break;
    }
    text[used] = '\0';
    if (platform::kSimulator) platform::log("input %s", text);
    return Edit::Changed;
  }
  if (event.action == first_ + 2) return Edit::Ok;
  return Edit::None;
}

}  // namespace tinta::ui
