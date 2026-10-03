#pragma once

// The results of the last search in a book, kept on SD at <book cache>/search.bin so the result list never
// has to fit in RAM: the screen reads the rows it shows, and Back from a result reopens the list without
// searching again. See docs/file-formats.md.

#include <cstddef>
#include <cstdint>
#include <string>

#include "BookSearchText.h"

class HalFile;

namespace booksearch {

enum class SearchState : uint8_t {
  // Scanning, or stopped before the end (a result was opened, the reader left): resumable.
  Running = 0,
  Complete = 1,
  // Stopped at MAX_RESULTS.
  Full = 2,
};

struct ResultsHeader {
  static constexpr uint32_t MAGIC = 0x3152534C;  // "LSR1"
  static constexpr uint8_t VERSION = 1;
  static constexpr size_t SIZE = 160;

  SearchState state = SearchState::Running;
  uint16_t spineCount = 0;
  // Spines before this one are fully searched.
  uint16_t nextSpine = 0;
  // Index of the first result in nextSpine. Results from there to `count` were found in a spine the search
  // left early; resuming finds them again, identically, before adding new ones.
  uint16_t spineFirstResult = 0;
  uint16_t count = 0;
  // The table-of-contents entry in force at the start of nextSpine.
  int16_t tagAtNextSpine = -1;
  char query[QUERY_MAX_BYTES + 1] = {};

  void serialize(uint8_t (&out)[SIZE]) const;
  bool deserialize(const uint8_t (&in)[SIZE]);
};

struct ResultRecord {
  static constexpr size_t SIZE = 208;
  static constexpr size_t EXCERPT_BYTES = ExcerptBuilder::MAX_BYTES;

  uint16_t spineIndex = 0;
  int16_t tocIndex = -1;  // the table-of-contents entry the match falls under, -1 for none
  uint32_t start = 0;     // visible-text offsets in the spine (see SpineTextScanner)
  uint32_t end = 0;
  uint8_t bookPercent = 0;
  char excerpt[EXCERPT_BYTES] = {};

  void serialize(uint8_t (&out)[SIZE]) const;
  void deserialize(const uint8_t (&in)[SIZE]);
};

// Most results one search keeps. A word on every page would otherwise cost minutes of SD writes for a list
// nobody reads to the end; a longer phrase narrows it.
constexpr uint16_t MAX_RESULTS = 500;

std::string resultsPath(const std::string& bookCachePath);
bool readHeader(const std::string& path, ResultsHeader& header);
// Reads results [first, first + count) into `out`. Returns how many were read.
size_t readResults(const std::string& path, uint16_t first, size_t count, ResultRecord* out);

}  // namespace booksearch
