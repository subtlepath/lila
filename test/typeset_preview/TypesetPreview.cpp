// Typeset preview: lays out XHTML chapters with the firmware's own parser, line breaker and built-in bitmap fonts,
// and writes each reader page as a PGM image at panel resolution. Bitmap glyphs are drawn 1:1, so a preview is the
// frame the X4 shows (minus the status bar, which is drawn as a placeholder).
//
//   TypesetPreview --family times --size 12 --out /tmp/pages [--css book.css]... [--set key=value]... ch1.xhtml ...
//
// --stats FILE appends one JSON object per laid-out line (page, y, available width, alignment, and each word's text,
// x and advance) for preview.py to score.
//
// Defaults are the firmware's: designed leading, measure and alignment (src/ReaderTypography), book paragraphs.
// --set keys mirror ReaderRenderSpec / CrossPointSettings: lineSpacing (0-3, a step of the designed leading),
// lineCompression (explicit multiplier instead), margin, wordSpacing, tracking, paragraphSpacing, alignment
// (4 = book style, resolved by measure as the firmware does), hyphenation, embeddedStyle, lang, orientation
// (portrait|landscape), maxPages, statusBar (reserved px below the text).

#include <Epub/Page.h>
#include <Epub/css/CssParser.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <builtinFonts/all.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "src/ReaderTypography.h"
#include "src/fontIds.h"

namespace {

struct Options {
  std::string family = "times";
  int size = 12;
  std::string outDir = ".";
  std::string statsPath;
  std::vector<std::string> cssFiles;
  std::vector<std::string> chapters;
  int lineSpacing = 2;            // WIDE, the default step
  float lineCompression = -1.0f;  // < 0: designed leading at lineSpacing
  int margin = 5;
  int wordSpacing = 100;
  int tracking = 0;
  bool paragraphSpacing = false;
  int alignment = 4;
  bool hyphenation = true;
  bool embeddedStyle = true;
  std::string lang = "en";
  bool landscape = false;
  int maxPages = 1000;
  int statusBar = 14;  // Lyra statusBarVerticalMargin with the text lane on
};

// X4 bezel insets in the panel's portrait frame (BoardConfig ViewableInsets defaults).
constexpr int INSET_TOP = 9;
constexpr int INSET_RIGHT = 3;
constexpr int INSET_BOTTOM = 3;
constexpr int INSET_LEFT = 3;

struct FontSet {
  EpdFont regular, bold, italic, boldItalic;
  FontSet(const EpdFontData* r, const EpdFontData* b, const EpdFontData* i, const EpdFontData* bi)
      : regular(r), bold(b), italic(i), boldItalic(bi) {}
  EpdFontFamily family() const { return EpdFontFamily(&regular, &bold, &italic, &boldItalic); }
};

#define FONT_SET(name) FontSet(&name##_regular, &name##_bold, &name##_italic, &name##_bolditalic)

int registerFonts(GfxRenderer& renderer, const Options& options) {
  static FontSet sets[] = {FONT_SET(times_8),      FONT_SET(times_9),     FONT_SET(times_12),    FONT_SET(times_14),
                           FONT_SET(times_16),     FONT_SET(helvetica_8), FONT_SET(helvetica_9), FONT_SET(helvetica_12),
                           FONT_SET(helvetica_14), FONT_SET(helvetica_16)};
  static const int ids[] = {TIMES_8_FONT_ID,      TIMES_9_FONT_ID,     TIMES_12_FONT_ID,    TIMES_14_FONT_ID,
                            TIMES_16_FONT_ID,     HELVETICA_8_FONT_ID, HELVETICA_9_FONT_ID, HELVETICA_12_FONT_ID,
                            HELVETICA_14_FONT_ID, HELVETICA_16_FONT_ID};
  static const int sizes[] = {8, 9, 12, 14, 16};
  for (size_t i = 0; i < std::size(ids); ++i) renderer.insertFont(ids[i], sets[i].family());
  const size_t familyBase = options.family == "helvetica" ? 5 : 0;
  for (size_t s = 0; s < std::size(sizes); ++s) {
    if (sizes[s] == options.size) return ids[familyBase + s];
  }
  std::fprintf(stderr, "no built-in size %d\n", options.size);
  std::exit(2);
}

bool parseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", arg.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--family") {
      options.family = next();
    } else if (arg == "--size") {
      options.size = std::atoi(next().c_str());
    } else if (arg == "--out") {
      options.outDir = next();
    } else if (arg == "--stats") {
      options.statsPath = next();
    } else if (arg == "--css") {
      options.cssFiles.push_back(next());
    } else if (arg == "--set") {
      const std::string kv = next();
      const size_t eq = kv.find('=');
      if (eq == std::string::npos) return false;
      const std::string key = kv.substr(0, eq);
      const std::string value = kv.substr(eq + 1);
      if (key == "lineCompression")
        options.lineCompression = std::strtof(value.c_str(), nullptr);
      else if (key == "lineSpacing")
        options.lineSpacing = std::atoi(value.c_str());
      else if (key == "margin")
        options.margin = std::atoi(value.c_str());
      else if (key == "wordSpacing")
        options.wordSpacing = std::atoi(value.c_str());
      else if (key == "tracking")
        options.tracking = std::atoi(value.c_str());
      else if (key == "paragraphSpacing")
        options.paragraphSpacing = value != "0";
      else if (key == "alignment")
        options.alignment = std::atoi(value.c_str());
      else if (key == "hyphenation")
        options.hyphenation = value != "0";
      else if (key == "embeddedStyle")
        options.embeddedStyle = value != "0";
      else if (key == "lang")
        options.lang = value;
      else if (key == "orientation")
        options.landscape = value == "landscape";
      else if (key == "maxPages")
        options.maxPages = std::atoi(value.c_str());
      else if (key == "statusBar")
        options.statusBar = std::atoi(value.c_str());
      else {
        std::fprintf(stderr, "unknown key %s\n", key.c_str());
        return false;
      }
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", arg.c_str());
      return false;
    } else {
      options.chapters.push_back(arg);
    }
  }
  return !options.chapters.empty();
}

void writeJsonString(std::FILE* file, const char* text) {
  std::fputc('"', file);
  for (const char* c = text; *c; ++c) {
    const auto byte = static_cast<unsigned char>(*c);
    if (byte == '"' || byte == '\\') {
      std::fputc('\\', file);
      std::fputc(byte, file);
    } else if (byte < 0x20) {
      std::fprintf(file, "\\u%04x", byte);
    } else {
      std::fputc(byte, file);
    }
  }
  std::fputc('"', file);
}

void writeLineStats(std::FILE* file, const GfxRenderer& renderer, const int fontId, const int page,
                    const PageLine& line, const int viewportWidth) {
  const TextBlock& block = *line.getBlock();
  const BlockStyle& style = block.getBlockStyle();
  std::fprintf(file, "{\"page\":%d,\"y\":%d,\"x\":%d,\"avail\":%d,\"align\":%d,\"words\":[", page, line.yPos, line.xPos,
               viewportWidth - style.totalHorizontalInset(), static_cast<int>(style.alignment));
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    const int advance = renderer.getTextAdvanceX(fontId, block.wordText(i), block.wordStyle(i), style.characterSpacing);
    std::fprintf(file, "%s[", i ? "," : "");
    writeJsonString(file, block.wordText(i));
    std::fprintf(file, ",%d,%d,%d]", block.wordXpos(i), advance, static_cast<int>(block.wordStyle(i)));
  }
  std::fprintf(file, "]}\n");
}

bool writePgm(const std::string& path, const GfxRenderer& renderer) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  std::fprintf(file, "P5\n%d %d\n255\n", renderer.getScreenWidth(), renderer.getScreenHeight());
  const auto& pixels = renderer.canvas();
  const bool ok = std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size();
  std::fclose(file);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseArgs(argc, argv, options)) {
    std::fprintf(stderr,
                 "usage: TypesetPreview [--family times|helvetica] [--size N] [--out DIR] [--css FILE]... "
                 "[--set key=value]... chapter.xhtml...\n");
    return 2;
  }

  GfxRenderer renderer;
  const int fontId = registerFonts(renderer, options);
  if (options.landscape) {
    renderer.setCanvas(800, 480);
  } else {
    renderer.setCanvas(480, 800);
  }

  // EpubReaderActivity::render(): bezel insets plus the user margin; the bottom also clears the status bar.
  // Landscape rotates the portrait insets clockwise.
  const int insetTop = options.landscape ? INSET_LEFT : INSET_TOP;
  const int insetRight = options.landscape ? INSET_TOP : INSET_RIGHT;
  const int insetBottom = options.landscape ? INSET_RIGHT : INSET_BOTTOM;
  const int insetLeft = options.landscape ? INSET_BOTTOM : INSET_LEFT;
  const int marginTop = insetTop + options.margin;
  const int measureInset = ReaderTypography::measureInset(
      renderer, fontId, renderer.getScreenWidth() - insetLeft - insetRight - 2 * options.margin);
  const int marginLeft = insetLeft + options.margin + measureInset;
  const int marginRight = insetRight + options.margin + measureInset;
  const int marginBottom = insetBottom + std::max(options.margin, options.statusBar);
  const auto viewportWidth = static_cast<uint16_t>(renderer.getScreenWidth() - marginLeft - marginRight);
  const auto viewportHeight = static_cast<uint16_t>(renderer.getScreenHeight() - marginTop - marginBottom);

  // CrossPointSettings::getReaderLineCompression() and readerRenderSpec().
  if (options.lineCompression < 0) {
    const int pitch =
        ReaderTypography::builtinLinePitch(options.family == "helvetica", options.size, options.lineSpacing);
    options.lineCompression = (static_cast<float>(pitch) + 0.25f) / static_cast<float>(renderer.getLineHeight(fontId));
  }
  if (options.alignment == 4 && !ReaderTypography::measureJustifies(renderer, fontId, viewportWidth)) {
    options.alignment = 1;
  }

  CssParser cssParser{std::filesystem::temp_directory_path().string()};
  for (const auto& css : options.cssFiles) {
    HalFile file;
    if (!file.open(css.c_str(), "rb")) {
      std::fprintf(stderr, "cannot open %s\n", css.c_str());
      return 1;
    }
    cssParser.loadFromStream(file);
  }

  Hyphenator::setPreferredLanguage(options.lang);
  std::filesystem::create_directories(options.outDir);

  std::FILE* stats = options.statsPath.empty() ? nullptr : std::fopen(options.statsPath.c_str(), "a");
  int pageNumber = 0;
  int lineCount = 0;
  for (const auto& chapter : options.chapters) {
    std::vector<std::unique_ptr<Page>> pages;
    const std::string contentBase = std::filesystem::path(chapter).parent_path().string() + "/";
    ChapterHtmlSlimParser parser(
        nullptr, chapter, renderer, fontId, options.lineCompression, options.paragraphSpacing,
        static_cast<uint8_t>(options.alignment), viewportWidth, viewportHeight, options.hyphenation, false,
        [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); },
        options.embeddedStyle, contentBase, "", 0, {}, nullptr, options.embeddedStyle ? &cssParser : nullptr);
    parser.setTextSpacing(static_cast<int8_t>(options.tracking), static_cast<uint8_t>(options.wordSpacing));
    if (!parser.parseAndBuildPages()) {
      std::fprintf(stderr, "parse failed: %s\n", chapter.c_str());
      return 1;
    }

    for (size_t i = 0; i < pages.size() && pageNumber < options.maxPages; ++i) {
      renderer.clearScreen();
      pages[i]->render(renderer, fontId, marginLeft, marginTop);
      for (const auto& element : pages[i]->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        ++lineCount;
        if (stats) {
          writeLineStats(stats, renderer, fontId, pageNumber + 1, static_cast<const PageLine&>(*element),
                         viewportWidth);
        }
      }
      char path[512];
      std::snprintf(path, sizeof(path), "%s/page-%04d.pgm", options.outDir.c_str(), ++pageNumber);
      if (!writePgm(path, renderer)) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
      }
    }
  }
  if (stats) std::fclose(stats);
  std::printf("pages=%d lines=%d viewport=%ux%u margins=%d,%d,%d,%d lineHeight=%d align=%d\n", pageNumber, lineCount,
              viewportWidth, viewportHeight, marginTop, marginRight, marginBottom, marginLeft,
              renderer.getLineHeight(fontId, options.lineCompression), options.alignment);
  return 0;
}
