# The search text model and chapter scanner, plus the firmware's layout parser: a match's offset must land on
# the page that shows it.
add_executable(BookSearchTest
  BookSearchTest.cpp
  ParserLinkStubs.cpp
  ${REPO_ROOT}/lib/BookSearch/BookSearchResults.cpp
  ${REPO_ROOT}/lib/BookSearch/BookSearchText.cpp
  ${REPO_ROOT}/lib/BookSearch/HtmlTextRules.cpp
  ${REPO_ROOT}/lib/BookSearch/SpineTextScanner.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
)

target_include_directories(BookSearchTest PRIVATE
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/BookSearch
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/expat
)

# The firmware's expat configuration (platformio.ini).
target_compile_definitions(BookSearchTest PRIVATE
  XML_CONTEXT_BYTES=1024
  XML_GE=0
)

target_link_libraries(BookSearchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookSearchTest)
