FetchContent_Declare(arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2)
FetchContent_MakeAvailable(arduinojson)

add_executable(BookmarkFileTest BookmarkFileTest.cpp
  ${REPO_ROOT}/src/util/BookmarkFile.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
  ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp
  ${REPO_ROOT}/lib/Companion/CompanionIdentity.cpp
  ${REPO_ROOT}/lib/hal/HalTintaJournalStorage.cpp
  ${REPO_ROOT}/lib/hal/HalInventorySortStorage.cpp
  ${REPO_ROOT}/lib/hal/HalInventoryIndexStorage.cpp)
target_include_directories(BookmarkFileTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/companion/hal_transfer_stubs
  ${arduinojson_SOURCE_DIR}/src)
target_include_directories(BookmarkFileTest PRIVATE
  ${REPO_ROOT}/src ${REPO_ROOT}/lib/hal ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/Memory)
find_package(OpenSSL REQUIRED)
target_link_libraries(BookmarkFileTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(BookmarkFileTest)
