#pragma once

// Tap rects of the header's buttons on touch boards, recorded at draw time.
// Written by the render task and read by the loop task; a torn int read at
// worst misroutes one tap on a band that is being redrawn, so no lock is
// taken. Cleared on activity exit so a stale rect never eats taps on a new
// screen.
struct HeaderTapRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  void set(const int newX, const int newY, const int newW, const int newH) {
    x = newX;
    y = newY;
    w = newW;
    h = newH;
  }

  void clear() { w = 0; }

  bool contains(const int tx, const int ty) const { return w > 0 && tx >= x && tx < x + w && ty >= y && ty < y + h; }
};

// The back button BaseTheme::drawHeader paints. The header's FreeInkUI frame
// is non-interactive, so MappedInputManager folds taps on it into Back; every
// screen that draws a titled header gets tap-to-go-back without its own
// routing. Headerless draws clear it.
inline HeaderTapRect HeaderBackTapTarget;

// The trailing action buttons headerWithActions() draws (Home's Search and
// Menu). Their own screen routes the taps; this only keeps the status-band
// tap that opens the light panel from taking them first.
inline HeaderTapRect HeaderActionTapTarget;
