#include "ui/screens/Sheets.h"

#include <stdio.h>

#include "app/App.h"
#include "core/lang/Charset.h"
#include "icons/Icons.h"
#include "platform/Log.h"
#include "ui/Fonts.h"
#include "ui/Strings.h"
#include "ui/views/Chrome.h"

namespace tinta::ui {
namespace {

using app::App;
using app::ScreenId;
using freeink::ui::Insets;
using freeink::ui::Rect;
using RowKind = RowSpec::Kind;

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

enum class PauseItem : uint8_t { Title, Resume, EndSession, Home, Settings, Light, Sleep };

bool overSession(App& app) {
  return app.depth() >= 2 && app.idAt(static_cast<uint8_t>(app.depth() - 2)) == ScreenId::Session &&
         app.session().active();
}

// The rows shown, in order; End session over a session; Light only where
// there is one.
uint8_t pauseItems(App& app, PauseItem* out) {
  uint8_t n = 0;
  out[n++] = PauseItem::Title;
  out[n++] = PauseItem::Resume;
  if (overSession(app)) out[n++] = PauseItem::EndSession;
  out[n++] = PauseItem::Home;
  out[n++] = PauseItem::Settings;
  if (app.board().hasFrontlight()) out[n++] = PauseItem::Light;
  // Leaves for lila; its Power key is how the device sleeps.
  out[n++] = PauseItem::Sleep;
  return n;
}

enum : app::ActionId { kActionDismiss = app::kFirstViewAction + 8 };

// Content goes below `top` and above `bottom` (screen coordinates).
void constrainTo(app::UiScreen& screen, int16_t top, int16_t bottom) {
  const Rect safe = screen.frame().safeRect();
  screen.setContentMargin(
      Insets{i16(top > safe.y ? top - safe.y : 0), 0, i16(safe.bottom() > bottom ? safe.bottom() - bottom : 0), 0});
}

}  // namespace

// ── Pause ────────────────────────────────────────────────────────────────────

uint8_t PauseSheet::rowCount() const {
  PauseItem items[8];
  return pauseItems(app_, items);
}

void PauseSheet::row(const uint8_t index, RowSpec& out, char*, size_t) const {
  PauseItem items[8];
  const uint8_t n = pauseItems(app_, items);
  if (index >= n) return;
  out.kind = RowKind::Action;
  switch (items[index]) {
    case PauseItem::Title:
      out.kind = RowKind::Heading;
      out.label = tr(Str::Paused);
      break;
    case PauseItem::Resume:
      out.label = tr(Str::Resume);
      out.slug = "resume";
      break;
    case PauseItem::EndSession:
      out.label = tr(Str::EndSession);
      out.slug = "end";
      break;
    case PauseItem::Home:
      out.label = tr(Str::Home);
      out.slug = "home";
      break;
    case PauseItem::Settings:
      out.label = tr(Str::Settings);
      out.slug = "settings";
      break;
    case PauseItem::Light:
      out.label = tr(Str::Light);
      out.slug = "light";
      break;
    case PauseItem::Sleep:
      out.label = tr(Str::Leave);
      out.slug = "sleep";
      break;
  }
}

void PauseSheet::activate(const uint8_t index) {
  PauseItem items[8];
  const uint8_t n = pauseItems(app_, items);
  if (index >= n) return;
  app_.clearTapFlash();
  switch (items[index]) {
    case PauseItem::Title:
      break;
    case PauseItem::Resume:
      app_.pop();
      break;
    case PauseItem::EndSession: {
      app_.session().end();
      app_.saveSession();
      if (app_.session().summaryTotals().reviews == 0) {
        app_.goHome();
        break;
      }
      const ScreenId ids[] = {app_.rootId(), ScreenId::Summary};
      app_.resetTo(ids, 2, freeink::ui::RefreshHint::Full);
      break;
    }
    case PauseItem::Home:
      app_.goHome();
      break;
    case PauseItem::Settings: {
      const ScreenId ids[] = {app_.rootId(), ScreenId::Settings};
      app_.resetTo(ids, 2);
      break;
    }
    case PauseItem::Light:
      app_.openLight();
      break;
    case PauseItem::Sleep:
      app_.pop();
      app_.requestExit();
      break;
  }
}

void PauseSheet::build(app::UiScreen& screen) {
  const Theme& theme = app_.theme();
  freeink::ui::DrawTarget& t = screen.target();
  const Rect body = screen.body();
  const Rect full = screen.frame().screen();

  const uint8_t rows = rowCount();
  freeink::ui::SheetProps props;
  props.anchor = freeink::ui::SheetEdge::Bottom;
  props.dismissAction = kActionDismiss;
  props.radius = theme.tokens.sheetRadius;
  const int16_t band = i16(props.grabberMargin + props.grabberHeight + props.grabberInset);
  const int16_t height =
      i16(band + (t.lineHeight(kSlotSmall) + 14) + (rows - 1) * (theme.rowHeight + (theme.touch ? 4 : 0)) + theme.gap);
  const Rect sheet{full.x, i16(body.bottom() - height), full.width, height};
  freeink::ui::sheet(screen.frame(), sheet, props);
  const Rect content = freeink::ui::sheetContentRect(sheet, props);
  constrainTo(screen, content.y, content.bottom());
  buildRows(screen);
}

void PauseSheet::onAction(const app::ActionEvent& event) {
  if (event.action == kActionDismiss) {
    app_.clearTapFlash();
    app_.pop();
    return;
  }
  FormView::onAction(event);
}

}  // namespace tinta::ui
