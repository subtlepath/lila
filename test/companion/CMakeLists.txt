add_executable(CompanionFrameTest CompanionFrameTest.cpp)
target_link_libraries(CompanionFrameTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionFrameTest)

add_executable(CompanionBookmarkPublicationTest CompanionBookmarkPublicationTest.cpp)
target_link_libraries(CompanionBookmarkPublicationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionBookmarkPublicationTest)

add_executable(CompanionRecordsTest CompanionRecordsTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionRecordsTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionRecordsTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
gtest_discover_tests(CompanionRecordsTest)

add_executable(CompanionCommandQueueTest CompanionCommandQueueTest.cpp)
target_link_libraries(CompanionCommandQueueTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionCommandQueueTest)

add_executable(CompanionTransferTest CompanionTransferTest.cpp
  ../../lib/Companion/CompanionTransfer.cpp ../../lib/Companion/CompanionRecords.cpp
  ../../lib/Companion/CompanionTransferHandler.cpp)
target_link_libraries(CompanionTransferTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionTransferTest PRIVATE LILA_TINTA=1)
gtest_discover_tests(CompanionTransferTest)

add_executable(CompanionIdentityTest CompanionIdentityTest.cpp ../../lib/Companion/CompanionIdentity.cpp)
target_include_directories(CompanionIdentityTest PRIVATE ${REPO_ROOT}/test/koreader_xpath_resolver/stubs
  ${REPO_ROOT}/lib/Companion)
target_link_libraries(CompanionIdentityTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionIdentityTest)

add_executable(CompanionTransferCommandsTest CompanionTransferCommandsTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionTransferCommandsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionTransferCommandsTest)

add_executable(CompanionSessionTest CompanionSessionTest.cpp)
target_link_libraries(CompanionSessionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionSessionTest)

add_executable(CompanionPairingsTest CompanionPairingsTest.cpp ../../lib/Companion/CompanionPairings.cpp)
target_link_libraries(CompanionPairingsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionPairingsTest)

add_executable(CompanionTintaBodyTest CompanionTintaBodyTest.cpp)
target_sources(CompanionTintaBodyTest PRIVATE
  ${REPO_ROOT}/lib/Tinta/src/core/pack/Pack.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/pack/PackSource.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/pack/Fold.cpp)
target_compile_definitions(CompanionTintaBodyTest PRIVATE TINTA_TEST_PACK="${REPO_ROOT}/test/tinta/fixtures/sim-fixture.pack")
target_include_directories(CompanionTintaBodyTest PRIVATE ${REPO_ROOT}/lib/Tinta/src)
target_link_libraries(CompanionTintaBodyTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionTintaBodyTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
gtest_discover_tests(CompanionTintaBodyTest)

add_executable(HalTintaDayLogValidationTest HalTintaDayLogValidationTest.cpp)
target_sources(HalTintaDayLogValidationTest PRIVATE
  ${REPO_ROOT}/lib/Tinta/src/core/srs/ItemState.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/profile/Profile.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/pack/Pack.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/pack/PackSource.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/pack/Fold.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/library/MarkLog.cpp)
target_compile_definitions(HalTintaDayLogValidationTest PRIVATE TINTA_TEST_PACK="${REPO_ROOT}/test/tinta/fixtures/sim-fixture.pack")
add_executable(CompanionTintaDerivedPublicationTest CompanionTintaDerivedPublicationTest.cpp)
target_include_directories(CompanionTintaDerivedPublicationTest PRIVATE ${REPO_ROOT}/lib/Tinta/src)
target_compile_definitions(CompanionTintaDerivedPublicationTest PRIVATE TINTA_DERIVED_FIXTURE="${REPO_ROOT}/protocol/fixtures/TintaDerivedManifest-v1.fixture")
target_link_libraries(CompanionTintaDerivedPublicationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionTintaDerivedPublicationTest)
target_include_directories(HalTintaDayLogValidationTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/Tinta/src)
target_link_libraries(HalTintaDayLogValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalTintaDayLogValidationTest)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
target_sources(HalTintaDayLogValidationTest PRIVATE ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp)
target_link_libraries(HalTintaDayLogValidationTest PRIVATE OpenSSL::Crypto)
add_executable(CompanionTintaJournalTest CompanionTintaJournalTest.cpp ../../lib/Companion/CompanionRecords.cpp ../../lib/Companion/CompanionIdentity.cpp)
target_compile_definitions(CompanionTintaJournalTest PRIVATE TINTA_ITEM_SNAPSHOT_FIXTURE="${REPO_ROOT}/protocol/fixtures/TintaItemSnapshot-v1.fixture")
target_compile_definitions(CompanionTintaJournalTest PRIVATE TINTA_AUTHORITY_CHECKPOINT_FIXTURE="${REPO_ROOT}/protocol/fixtures/TintaAuthorityCheckpoint-v1.fixture")
target_compile_definitions(CompanionTintaJournalTest PRIVATE TINTA_APPLICATION_RECEIPT_FIXTURE="${REPO_ROOT}/protocol/fixtures/TintaApplicationReceipt-v1.fixture")
target_compile_definitions(CompanionTintaJournalTest PRIVATE JOURNAL_EXPORT_PAGE_FIXTURE="${REPO_ROOT}/protocol/fixtures/JournalExportPage-v1.fixture")
target_sources(CompanionTintaJournalTest PRIVATE
  ${REPO_ROOT}/lib/Tinta/src/core/library/MarkLog.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/srs/ProgressStore.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/srs/Fsrs.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/srs/ItemState.cpp
  ${REPO_ROOT}/lib/Tinta/src/core/srs/Review.cpp)
target_include_directories(CompanionTintaJournalTest PRIVATE ${REPO_ROOT}/lib/Tinta/src)
target_link_libraries(CompanionTintaJournalTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(CompanionTintaJournalTest)

add_executable(HalTintaJournalStorageTest HalTintaJournalStorageTest.cpp
  ../../lib/Companion/CompanionIdentity.cpp
  ../../lib/Tinta/src/core/profile/Profile.cpp
  ../../lib/Tinta/src/core/srs/ProgressStore.cpp
  ../../lib/hal/HalInventoryFileHash.cpp
  ../../lib/hal/HalInventoryFileHash.cpp
  ../../lib/Tinta/src/core/srs/ItemState.cpp ../../lib/Tinta/src/core/srs/Fsrs.cpp ../../lib/Tinta/src/core/srs/Review.cpp
  ../../lib/hal/HalTintaJournalStorage.cpp ../../lib/hal/HalInventorySortStorage.cpp ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalTintaJournalStorageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/hal ${REPO_ROOT}/lib/Tinta/src ${REPO_ROOT}/lib/Memory)
target_link_libraries(HalTintaJournalStorageTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
target_compile_definitions(HalTintaJournalStorageTest PRIVATE TINTA_FRONTIER_FIXTURE="${REPO_ROOT}/protocol/fixtures/TintaJournalFrontier-v1.fixture")
gtest_discover_tests(HalTintaJournalStorageTest)

add_executable(HalCompanionWifiCipherTest HalCompanionWifiCipherTest.cpp ../../lib/hal/HalCompanionWifiCipher.cpp)
target_include_directories(HalCompanionWifiCipherTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs
  ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/hal)
target_compile_definitions(HalCompanionWifiCipherTest PRIVATE WIFI_FIXTURE="${REPO_ROOT}/protocol/fixtures/WifiMessage.json")
target_link_libraries(HalCompanionWifiCipherTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalCompanionWifiCipherTest)

add_executable(CompanionWifiHandoffTest CompanionWifiHandoffTest.cpp)
target_compile_definitions(CompanionWifiHandoffTest PRIVATE WIFI_HANDOFF_FIXTURE="${REPO_ROOT}/protocol/fixtures/WifiHandoffOffer.json")
target_link_libraries(CompanionWifiHandoffTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionWifiHandoffTest)

add_executable(CompanionInventoryTest CompanionInventoryTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionInventoryTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionInventoryTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
gtest_discover_tests(CompanionInventoryTest)

add_executable(CompanionInventoryIndexTest CompanionInventoryIndexTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionInventoryIndexTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionInventoryIndexTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
gtest_discover_tests(CompanionInventoryIndexTest)

add_executable(HalInventoryIndexStorageTest HalInventoryIndexStorageTest.cpp ../../lib/hal/HalInventoryIndexStorage.cpp)
target_include_directories(HalInventoryIndexStorageTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalInventoryIndexStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalInventoryIndexStorageTest)

add_executable(CompanionInventoryIndexBuilderTest CompanionInventoryIndexBuilderTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionInventoryIndexBuilderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionInventoryIndexBuilderTest)

add_executable(HalInventoryIndexSinkTest ../../lib/hal/HalInventoryRevisions.cpp HalInventoryIndexSinkTest.cpp ../../lib/hal/HalInventoryIndexSink.cpp ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalInventoryIndexSinkTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_sink_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalInventoryIndexSinkTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalInventoryIndexSinkTest)

file(GLOB COMPANION_COURSE_SOURCES ${REPO_ROOT}/lib/Tinta/src/core/pack/*.cpp)
add_executable(CompanionCourseValidationTest CompanionCourseValidationTest.cpp ../../lib/hal/HalCourseValidation.cpp ../../lib/hal/HalInventoryIndexStorage.cpp ${COMPANION_COURSE_SOURCES})
target_include_directories(CompanionCourseValidationTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_sink_stubs ${REPO_ROOT}/lib/Companion)
target_include_directories(CompanionCourseValidationTest PRIVATE ${REPO_ROOT}/lib/Tinta/src)
target_compile_definitions(CompanionCourseValidationTest PRIVATE LILA_TINTA=1 COURSE_FIXTURE="${REPO_ROOT}/test/tinta/fixtures/mini.pack")
target_link_libraries(CompanionCourseValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionCourseValidationTest)

add_executable(CompanionIdentityUniquenessTest CompanionIdentityUniquenessTest.cpp)
target_link_libraries(CompanionIdentityUniquenessTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionIdentityUniquenessTest)

# Exercise the same UTF-16 filename comparison as firmware.
FetchContent_Declare(companion_sdfat
  GIT_REPOSITORY https://github.com/greiman/SdFat.git
  GIT_TAG 2.3.1)
FetchContent_GetProperties(companion_sdfat)
if(NOT companion_sdfat_POPULATED)
  FetchContent_Populate(companion_sdfat)
endif()

add_executable(HalCourseTransferTest HalCourseTransferTest.cpp
  ${REPO_ROOT}/src/activities/tinta/platform/StateFiles.cpp
  ../../lib/hal/HalCourseStateMigration.cpp
  ../../lib/hal/HalTransferStorage.cpp ../../lib/hal/HalInventoryFileHash.cpp ../../lib/hal/HalCourseValidation.cpp
  ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/Companion/CompanionTransfer.cpp
  ../../lib/Companion/CompanionRecords.cpp ../../lib/Companion/CompanionTransferHandler.cpp ${COMPANION_COURSE_SOURCES})
target_sources(HalCourseTransferTest PRIVATE ../../lib/Companion/CompanionIdentity.cpp
  ../../lib/hal/HalTintaJournalStorage.cpp ../../lib/Tinta/src/core/srs/ItemState.cpp
  ../../lib/Tinta/src/core/srs/Fsrs.cpp ../../lib/Tinta/src/core/srs/Review.cpp)
target_sources(HalCourseTransferTest PRIVATE ../../lib/hal/HalInventoryRevisions.cpp
  ../../lib/hal/HalInventorySortStorage.cpp ../../lib/hal/HalInventoryPublicationValidator.cpp)
target_sources(HalCourseTransferTest PRIVATE ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_sources(HalCourseTransferTest PRIVATE ${companion_sdfat_SOURCE_DIR}/src/common/FsUtf.cpp
  ${companion_sdfat_SOURCE_DIR}/src/common/upcase.cpp)
target_include_directories(HalCourseTransferTest PRIVATE ${companion_sdfat_SOURCE_DIR}/src)
target_include_directories(HalCourseTransferTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(HalCourseTransferTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures/")
target_include_directories(HalCourseTransferTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_sink_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs
  ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/Memory)
target_include_directories(HalCourseTransferTest PRIVATE ${REPO_ROOT}/lib/Tinta/src)
target_compile_definitions(HalCourseTransferTest PRIVATE LILA_TINTA=1 COURSE_FIXTURE="${REPO_ROOT}/test/tinta/fixtures/mini.pack")
target_link_libraries(HalCourseTransferTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalCourseTransferTest)

add_executable(TintaStateFilesTest TintaStateFilesTest.cpp ${REPO_ROOT}/src/activities/tinta/platform/StateFiles.cpp)
target_include_directories(TintaStateFilesTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs
  ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/Tinta/src)
target_compile_definitions(TintaStateFilesTest PRIVATE LILA_TINTA=1)
target_link_libraries(TintaStateFilesTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TintaStateFilesTest)

add_executable(CompanionTransferDisabledTest CompanionTransferDisabledTest.cpp
  ../../lib/Companion/CompanionTransfer.cpp ../../lib/Companion/CompanionRecords.cpp
  ../../lib/Companion/CompanionTransferHandler.cpp)
target_link_libraries(CompanionTransferDisabledTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionTransferDisabledTest PRIVATE LILA_TINTA=0)
gtest_discover_tests(CompanionTransferDisabledTest)

add_executable(CompanionTransferPsramTest CompanionTransferDisabledTest.cpp
  ../../lib/Companion/CompanionTransfer.cpp ../../lib/Companion/CompanionRecords.cpp
  ../../lib/Companion/CompanionTransferHandler.cpp)
target_link_libraries(CompanionTransferPsramTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionTransferPsramTest PRIVATE LILA_TINTA=0 BOARD_HAS_PSRAM=1)
gtest_discover_tests(CompanionTransferPsramTest)


add_executable(CompanionInventorySorterTest CompanionInventorySorterTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionInventorySorterTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionInventorySorterTest)

add_executable(HalInventorySortStorageTest HalInventorySortStorageTest.cpp
  ../../lib/hal/HalInventorySortStorage.cpp ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalInventorySortStorageTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs
  ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/Tinta/src)
target_link_libraries(HalInventorySortStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalInventorySortStorageTest)

add_executable(HalInventoryDirectoryWalkerTest HalInventoryDirectoryWalkerTest.cpp)
target_include_directories(HalInventoryDirectoryWalkerTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_directory_stubs)
target_link_libraries(HalInventoryDirectoryWalkerTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalInventoryDirectoryWalkerTest)

add_executable(HalInventoryFileHashTest HalInventoryFileHashTest.cpp ../../lib/hal/HalInventoryFileHash.cpp)
target_sources(HalInventoryFileHashTest PRIVATE ${REPO_ROOT}/lib/Tinta/src/core/srs/ItemState.cpp)
target_include_directories(HalInventoryFileHashTest PRIVATE ${REPO_ROOT}/lib/Tinta/src ${REPO_ROOT}/lib/Memory)
target_compile_definitions(HalInventoryFileHashTest PRIVATE TINTA_DERIVED_FIXTURE="${REPO_ROOT}/protocol/fixtures/TintaDerivedManifest-v1.fixture")
target_compile_definitions(HalInventoryFileHashTest PRIVATE READER_FONT_FIXTURE="${REPO_ROOT}/protocol/fixtures/BitmapFont-v4.fixture")
target_compile_definitions(HalInventoryFileHashTest PRIVATE READER_VECTOR_FONT_FIXTURE="${REPO_ROOT}/protocol/fixtures/VectorFont-sfnt.fixture")
target_include_directories(HalInventoryFileHashTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalInventoryFileHashTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalInventoryFileHashTest)

add_executable(HalInventoryFileSourceTest HalInventoryFileSourceTest.cpp ../../lib/hal/HalInventoryFileHash.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalInventoryFileSourceTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_directory_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalInventoryFileSourceTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalInventoryFileSourceTest)

add_executable(CompanionInventoryPathsTest CompanionInventoryPathsTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionInventoryPathsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionInventoryPathsTest)

add_executable(CompanionInventoryPathsBuilderTest CompanionInventoryPathsBuilderTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionInventoryPathsBuilderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionInventoryPathsBuilderTest)

add_executable(HalInventoryPathsStageTest HalInventoryPathsStageTest.cpp ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalInventoryPathsStageTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalInventoryPathsStageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalInventoryPathsStageTest)

add_executable(CompanionInventoryPublicationTest CompanionInventoryPublicationTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionInventoryPublicationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionInventoryPublicationTest)

add_executable(CompanionInventoryPairValidationTest CompanionInventoryPairValidationTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionInventoryPairValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionInventoryPairValidationTest)

add_executable(HalInventoryPublicationValidatorTest ../../lib/hal/HalInventoryRecovery.cpp ../../lib/hal/HalTransferStorage.cpp ../../lib/hal/HalInventoryFileHash.cpp HalInventoryPublicationValidatorTest.cpp ../../lib/hal/HalInventoryPublicationValidator.cpp ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalInventoryPublicationValidatorTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${REPO_ROOT}/lib/Companion)
target_compile_definitions(HalInventoryPublicationValidatorTest PRIVATE LILA_TINTA=0)
target_include_directories(HalInventoryPublicationValidatorTest PRIVATE ${REPO_ROOT}/lib/Memory)
target_link_libraries(HalInventoryPublicationValidatorTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
target_sources(HalInventoryPublicationValidatorTest PRIVATE ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalInventoryPublicationValidatorTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
gtest_discover_tests(HalInventoryPublicationValidatorTest)

add_executable(HalInventoryIndexStageTest HalInventoryIndexStageTest.cpp ../../lib/hal/HalInventoryPublicationValidator.cpp ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/hal/HalTransferStorage.cpp ../../lib/hal/HalInventoryFileHash.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalInventoryIndexStageTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/Memory)
target_compile_definitions(HalInventoryIndexStageTest PRIVATE LILA_TINTA=0)
target_link_libraries(HalInventoryIndexStageTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
target_sources(HalInventoryIndexStageTest PRIVATE ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalInventoryIndexStageTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
gtest_discover_tests(HalInventoryIndexStageTest)

add_executable(HalInventoryBuildTest HalInventoryBuildTest.cpp ../../lib/hal/HalInventoryRevisions.cpp ../../lib/hal/HalInventorySortStorage.cpp ../../lib/hal/HalInventoryPublicationValidator.cpp ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/hal/HalTransferStorage.cpp ../../lib/hal/HalInventoryFileHash.cpp ../../lib/Companion/CompanionRecords.cpp)
target_sources(HalInventoryBuildTest PRIVATE ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalInventoryBuildTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_include_directories(HalInventoryBuildTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_sink_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/Memory)
target_compile_definitions(HalInventoryBuildTest PRIVATE LILA_TINTA=0 VECTOR_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures/" FONT_FIXTURE="${REPO_ROOT}/protocol/fixtures/BitmapFont-v4.fixture")
target_link_libraries(HalInventoryBuildTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalInventoryBuildTest)

add_executable(CompanionBitmapFontValidationTest CompanionBitmapFontValidationTest.cpp)
target_compile_definitions(CompanionBitmapFontValidationTest PRIVATE FONT_FIXTURE="${REPO_ROOT}/protocol/fixtures/BitmapFont-v4.fixture")
target_link_libraries(CompanionBitmapFontValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionBitmapFontValidationTest)

add_executable(CompanionVectorFontValidationTest CompanionVectorFontValidationTest.cpp)
target_compile_definitions(CompanionVectorFontValidationTest PRIVATE FONT_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures/" REPO_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source/")
target_link_libraries(CompanionVectorFontValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionVectorFontValidationTest)

add_executable(CompanionDictionaryBundleBuilderTest CompanionDictionaryBundleBuilderTest.cpp)
target_compile_definitions(CompanionDictionaryBundleBuilderTest PRIVATE BUNDLE_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures/")
target_link_libraries(CompanionDictionaryBundleBuilderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryBundleBuilderTest)

add_executable(CompanionDictionaryIndexValidationTest CompanionDictionaryIndexValidationTest.cpp)
target_include_directories(CompanionDictionaryIndexValidationTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_compile_definitions(CompanionDictionaryIndexValidationTest PRIVATE INDEX_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures/")
target_link_libraries(CompanionDictionaryIndexValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryIndexValidationTest)

add_executable(CompanionDictionaryInfoValidationTest CompanionDictionaryInfoValidationTest.cpp)
target_compile_definitions(CompanionDictionaryInfoValidationTest PRIVATE INFO_FIXTURE="${REPO_ROOT}/protocol/fixtures/DictionaryInfo.fixture")
target_link_libraries(CompanionDictionaryInfoValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryInfoValidationTest)

add_executable(CompanionDictzipLayoutValidationTest CompanionDictzipLayoutValidationTest.cpp)
target_compile_definitions(CompanionDictzipLayoutValidationTest PRIVATE DICTZIP_FIXTURE="${REPO_ROOT}/protocol/fixtures/dictzip-chunks.dict.dz")
target_link_libraries(CompanionDictzipLayoutValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictzipLayoutValidationTest)

add_executable(CompanionDictzipValidationTest CompanionDictzipValidationTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(CompanionDictzipValidationTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(CompanionDictzipValidationTest PRIVATE DICTZIP_FIXTURE="${REPO_ROOT}/protocol/fixtures/dictzip-chunks.dict.dz")
target_link_libraries(CompanionDictzipValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictzipValidationTest)

add_executable(CompanionDictionaryMembersValidationTest CompanionDictionaryMembersValidationTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(CompanionDictionaryMembersValidationTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(CompanionDictionaryMembersValidationTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(CompanionDictionaryMembersValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryMembersValidationTest)

add_executable(HalDictionaryBundleSourceTest HalDictionaryBundleSourceTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalDictionaryBundleSourceTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(HalDictionaryBundleSourceTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalDictionaryBundleSourceTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalDictionaryBundleSourceTest)

add_executable(HalDictionaryDiscoveryTest HalDictionaryDiscoveryTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalDictionaryDiscoveryTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(HalDictionaryDiscoveryTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalDictionaryDiscoveryTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalDictionaryDiscoveryTest)

add_executable(HalDictionaryArchiveStageTest HalDictionaryArchiveStageTest.cpp
  ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalDictionaryArchiveStageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(HalDictionaryArchiveStageTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalDictionaryArchiveStageTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalDictionaryArchiveStageTest)

add_executable(CompanionDictionaryCachePublicationTest CompanionDictionaryCachePublicationTest.cpp)
target_compile_definitions(CompanionDictionaryCachePublicationTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(CompanionDictionaryCachePublicationTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(CompanionDictionaryCachePublicationTest)

add_executable(HalDictionaryCacheStorageTest HalDictionaryCacheStorageTest.cpp
  ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalDictionaryCacheStorageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/miniz/src ${REPO_ROOT}/lib/hal)
target_compile_definitions(HalDictionaryCacheStorageTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalDictionaryCacheStorageTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalDictionaryCacheStorageTest)

add_executable(HalInventoryDictionaryResolverTest HalInventoryDictionaryResolverTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp
  ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalInventoryDictionaryResolverTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(HalInventoryDictionaryResolverTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalInventoryDictionaryResolverTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalInventoryDictionaryResolverTest)

add_executable(CompanionDictionaryArchiveBindingTest CompanionDictionaryArchiveBindingTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionDictionaryArchiveBindingTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryArchiveBindingTest)

add_executable(HalDictionaryBindingsTest HalDictionaryBindingsTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp)
target_include_directories(HalDictionaryBindingsTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_compile_definitions(HalDictionaryBindingsTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalDictionaryBindingsTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalDictionaryBindingsTest)

add_executable(HalCompanionFileLookupTest HalCompanionFileLookupTest.cpp)
target_include_directories(HalCompanionFileLookupTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalCompanionFileLookupTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalCompanionFileLookupTest)

add_executable(CompanionZipEntryExtractionTest CompanionZipEntryExtractionTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(CompanionZipEntryExtractionTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(CompanionZipEntryExtractionTest PRIVATE ZIP_ENTRY_FIXTURE="${REPO_ROOT}/protocol/fixtures/ZipEntry-deflate.fixture")
target_link_libraries(CompanionZipEntryExtractionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipEntryExtractionTest)

add_executable(HalZipEntryStageTest HalZipEntryStageTest.cpp
  ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalZipEntryStageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(HalZipEntryStageTest PRIVATE ZIP_ENTRY_FIXTURE="${REPO_ROOT}/protocol/fixtures/ZipEntry-deflate.fixture")
target_link_libraries(HalZipEntryStageTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalZipEntryStageTest)

add_executable(CompanionZipDirectoryLayoutTest CompanionZipDirectoryLayoutTest.cpp)
target_compile_definitions(CompanionZipDirectoryLayoutTest PRIVATE ZIP_LAYOUT_FIXTURE="${REPO_ROOT}/protocol/fixtures/DictionaryBundle-plain.fixture")
target_link_libraries(CompanionZipDirectoryLayoutTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipDirectoryLayoutTest)

add_executable(CompanionZipPathValidationTest CompanionZipPathValidationTest.cpp)
target_link_libraries(CompanionZipPathValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipPathValidationTest)

add_executable(CompanionZipExtraValidationTest CompanionZipExtraValidationTest.cpp)
target_link_libraries(CompanionZipExtraValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipExtraValidationTest)

add_executable(CompanionZipDescriptorValidationTest CompanionZipDescriptorValidationTest.cpp)
target_link_libraries(CompanionZipDescriptorValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipDescriptorValidationTest)

add_executable(CompanionZipEntryMetadataValidationTest CompanionZipEntryMetadataValidationTest.cpp)
target_compile_definitions(CompanionZipEntryMetadataValidationTest PRIVATE ZIP_METADATA_FIXTURE="${REPO_ROOT}/protocol/fixtures/DictionaryBundle-plain.fixture")
target_link_libraries(CompanionZipEntryMetadataValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipEntryMetadataValidationTest)
target_include_directories(CompanionZipEntryMetadataValidationTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(CompanionZipEntryMetadataValidationTest PRIVATE ZIP_METADATA_DIR="${REPO_ROOT}/protocol/fixtures")

add_executable(CompanionZipArchiveValidationTest CompanionZipArchiveValidationTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(CompanionZipArchiveValidationTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(CompanionZipArchiveValidationTest PRIVATE ZIP_ARCHIVE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(CompanionZipArchiveValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipArchiveValidationTest)

add_executable(CompanionZipRangeValidationTest CompanionZipRangeValidationTest.cpp)
target_link_libraries(CompanionZipRangeValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipRangeValidationTest)

add_executable(HalZipRangeStorageTest HalZipRangeStorageTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
target_include_directories(HalZipRangeStorageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalZipRangeStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalZipRangeStorageTest)

target_include_directories(HalZipRangeStorageTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(HalZipRangeStorageTest PRIVATE ZIP_HAL_RANGE_DIR="${REPO_ROOT}/protocol/fixtures")

add_executable(CompanionZipLegacyNameTest CompanionZipLegacyNameTest.cpp)
target_compile_definitions(CompanionZipLegacyNameTest PRIVATE ZIP_CP437_FIXTURE="${REPO_ROOT}/protocol/fixtures/ZipName-cp437-utf8.fixture")
target_link_libraries(CompanionZipLegacyNameTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipLegacyNameTest)

add_executable(CompanionZipHeaderNameValidationTest CompanionZipHeaderNameValidationTest.cpp)
target_link_libraries(CompanionZipHeaderNameValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipHeaderNameValidationTest)

add_executable(CompanionUnicodeNfcTest CompanionUnicodeNfcTest.cpp)
target_compile_definitions(CompanionUnicodeNfcTest PRIVATE UNICODE_NFC_FIXTURE="${REPO_ROOT}/protocol/fixtures/Unicode-15.1-Normalization.fixture")
target_link_libraries(CompanionUnicodeNfcTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionUnicodeNfcTest)

add_executable(CompanionUnicodeUtf8NfcTest CompanionUnicodeUtf8NfcTest.cpp)
target_link_libraries(CompanionUnicodeUtf8NfcTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionUnicodeUtf8NfcTest)

add_executable(CompanionZipNameNormalizationTest CompanionZipNameNormalizationTest.cpp)
target_link_libraries(CompanionZipNameNormalizationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipNameNormalizationTest)

add_executable(CompanionZipNameDuplicateValidationTest CompanionZipNameDuplicateValidationTest.cpp)
target_link_libraries(CompanionZipNameDuplicateValidationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionZipNameDuplicateValidationTest)

add_executable(HalZipNameBytesStorageTest HalZipNameBytesStorageTest.cpp)
target_include_directories(HalZipNameBytesStorageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalZipNameBytesStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalZipNameBytesStorageTest)

add_executable(HalZipNormalizedArchiveTest HalZipNormalizedArchiveTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalZipNormalizedArchiveTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(HalZipNormalizedArchiveTest PRIVATE ZIP_NORMALIZED_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalZipNormalizedArchiveTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalZipNormalizedArchiveTest)

add_executable(CompanionDictionaryZipSelectionTest CompanionDictionaryZipSelectionTest.cpp)
target_include_directories(CompanionDictionaryZipSelectionTest PRIVATE ${REPO_ROOT}/lib/miniz/src)
target_link_libraries(CompanionDictionaryZipSelectionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryZipSelectionTest)

add_executable(CompanionDictionaryExtractionReceiptTest CompanionDictionaryExtractionReceiptTest.cpp)
target_link_libraries(CompanionDictionaryExtractionReceiptTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryExtractionReceiptTest)

add_executable(CompanionDictionaryExtractionJournalTest CompanionDictionaryExtractionJournalTest.cpp)
target_link_libraries(CompanionDictionaryExtractionJournalTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryExtractionJournalTest)

add_executable(CompanionDictionaryInstallationPlanTest CompanionDictionaryInstallationPlanTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionDictionaryInstallationPlanTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryInstallationPlanTest)

add_executable(CompanionDictionaryInstallationParentTest CompanionDictionaryInstallationParentTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionTransfer.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp
  ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp)
target_include_directories(CompanionDictionaryInstallationParentTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/miniz/src)
target_link_libraries(CompanionDictionaryInstallationParentTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(CompanionDictionaryInstallationParentTest)

add_executable(HalCompanionWifiDiscoveryTest HalCompanionWifiDiscoveryTest.cpp ../../lib/hal/HalCompanionWifiDiscovery.cpp)
target_include_directories(HalCompanionWifiDiscoveryTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_discovery_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/hal)
target_link_libraries(HalCompanionWifiDiscoveryTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalCompanionWifiDiscoveryTest)

add_executable(CompanionWifiRequestTest CompanionWifiRequestTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(CompanionWifiRequestTest PRIVATE ${REPO_ROOT}/lib/Tinta/src)
target_compile_definitions(CompanionWifiRequestTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(CompanionWifiRequestTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionWifiRequestTest)

add_executable(HalCompanionWifiMessagesTest HalCompanionWifiMessagesTest.cpp
  ../../lib/hal/HalCompanionWifiMessages.cpp ../../lib/hal/HalCompanionWifiCipher.cpp
  ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalCompanionWifiMessagesTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs
  ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/hal)
target_link_libraries(HalCompanionWifiMessagesTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalCompanionWifiMessagesTest)

add_executable(CompanionWifiHttpRequestTest CompanionWifiHttpRequestTest.cpp)
target_link_libraries(CompanionWifiHttpRequestTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionWifiHttpRequestTest)

add_executable(HalCompanionWifiHttpTest HalCompanionWifiHttpTest.cpp
  ../../lib/hal/HalCompanionWifiHttp.cpp ../../lib/hal/HalCompanionWifiMessages.cpp
  ../../lib/hal/HalCompanionWifiCipher.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalCompanionWifiHttpTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_socket_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/hal)
target_link_libraries(HalCompanionWifiHttpTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalCompanionWifiHttpTest)

add_executable(CompanionWifiSecretsTest CompanionWifiSecretsTest.cpp)
target_link_libraries(CompanionWifiSecretsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionWifiSecretsTest)

add_executable(CompanionWifiNetworkOfferTest CompanionWifiNetworkOfferTest.cpp)
target_compile_definitions(CompanionWifiNetworkOfferTest PRIVATE WIFI_NETWORK_FIXTURES="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(CompanionWifiNetworkOfferTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionWifiNetworkOfferTest)

add_executable(CompanionWifiHandoffCommandsTest CompanionWifiHandoffCommandsTest.cpp)
target_compile_definitions(CompanionWifiHandoffCommandsTest PRIVATE WIFI_HANDOFF_COMMAND_FIXTURE="${REPO_ROOT}/protocol/fixtures/WifiHandoffCommands.json")
target_link_libraries(CompanionWifiHandoffCommandsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionWifiHandoffCommandsTest)

add_executable(CompanionWifiHandoffLeaseTest CompanionWifiHandoffLeaseTest.cpp)
target_link_libraries(CompanionWifiHandoffLeaseTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionWifiHandoffLeaseTest)

add_executable(HalCompanionWifiSessionTest HalCompanionWifiSessionTest.cpp
  ../../lib/hal/HalCompanionWifiSession.cpp ../../lib/hal/HalCompanionWifiDiscovery.cpp
  ../../lib/hal/HalCompanionWifiHttp.cpp ../../lib/hal/HalCompanionWifiMessages.cpp
  ../../lib/hal/HalCompanionWifiCipher.cpp ../../lib/Companion/CompanionRecords.cpp)
target_include_directories(HalCompanionWifiSessionTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_session_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_discovery_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_socket_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs
  ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/hal)
target_link_libraries(HalCompanionWifiSessionTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
foreach(wifi_journal_target HalCompanionWifiSessionTest HalCompanionWifiMessagesTest HalCompanionWifiHttpTest)
  target_include_directories(${wifi_journal_target} PRIVATE ${REPO_ROOT}/lib/Tinta/src)
endforeach()
gtest_discover_tests(HalCompanionWifiSessionTest)

add_executable(HalCompanionWifiRadioTest HalCompanionWifiRadioTest.cpp ../../lib/hal/HalCompanionWifiRadio.cpp)
target_include_directories(HalCompanionWifiRadioTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_wifi_radio_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/hal)
target_link_libraries(HalCompanionWifiRadioTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalCompanionWifiRadioTest)

target_compile_definitions(CompanionTintaJournalTest PRIVATE TINTA_FRONTIER_FIXTURE="${REPO_ROOT}/protocol/fixtures/TintaJournalFrontier-v1.fixture")

target_include_directories(HalDictionaryArchiveStageTest PRIVATE "${REPO_ROOT}/lib/Tinta/src")

add_executable(CompanionCourseSwitchRequestTest CompanionCourseSwitchRequestTest.cpp)
target_link_libraries(CompanionCourseSwitchRequestTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionCourseSwitchRequestTest PRIVATE COURSE_SWITCH_FIXTURE="${REPO_ROOT}/protocol/fixtures/CourseSwitchRequest-v1.fixture")
gtest_discover_tests(CompanionCourseSwitchRequestTest)

add_executable(CompanionCourseSwitchIntentTest CompanionCourseSwitchIntentTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionCourseSwitchIntentTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionCourseSwitchIntentTest)

add_executable(CompanionFirmwareImageExtentTest CompanionFirmwareImageExtentTest.cpp)
target_link_libraries(CompanionFirmwareImageExtentTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionFirmwareImageExtentTest)

add_executable(CompanionFirmwareReaderInfoTest CompanionFirmwareReaderInfoTest.cpp)
target_link_libraries(CompanionFirmwareReaderInfoTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionFirmwareReaderInfoTest PRIVATE FIRMWARE_INFO_FIXTURE="${REPO_ROOT}/protocol/fixtures/FirmwareReaderInfo-v1.fixture")
gtest_discover_tests(CompanionFirmwareReaderInfoTest)
add_executable(CompanionFirmwareInstallRequestTest CompanionFirmwareInstallRequestTest.cpp)
target_link_libraries(CompanionFirmwareInstallRequestTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionFirmwareInstallRequestTest PRIVATE FIRMWARE_INSTALL_FIXTURE="${REPO_ROOT}/protocol/fixtures/FirmwareInstallRequest-v1.fixture")
gtest_discover_tests(CompanionFirmwareInstallRequestTest)
add_executable(CompanionFirmwareInstallIntentTest CompanionFirmwareInstallIntentTest.cpp)
target_link_libraries(CompanionFirmwareInstallIntentTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionFirmwareInstallIntentTest)

target_compile_definitions(HalCourseTransferTest PRIVATE FONT_FIXTURE="${REPO_ROOT}/protocol/fixtures/BitmapFont-v4.fixture")

foreach(font_board c3 s3)
  set(font_target HalCompanionFontInstallation_${font_board}Test)
  add_executable(${font_target} HalCompanionFontInstallationTest.cpp)
  target_include_directories(${font_target} BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/hal
    ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/Memory)
  target_compile_definitions(${font_target} PRIVATE
    VECTOR_FONT_FIXTURE="${REPO_ROOT}/protocol/fixtures/VectorFont-sfnt.fixture")
  if(font_board STREQUAL "s3")
    target_compile_definitions(${font_target} PRIVATE BOARD_HAS_PSRAM=1)
  endif()
  target_link_libraries(${font_target} PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(${font_target})
endforeach()

add_executable(CompanionDictionaryZipAuditTest CompanionDictionaryZipAuditTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionDictionaryZipAuditTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryZipAuditTest)

add_executable(CompanionContentRemovalJournalTest CompanionContentRemovalJournalTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionContentRemovalJournalTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionContentRemovalJournalTest)

add_executable(HalContentRemovalJournalStorageTest HalContentRemovalJournalStorageTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalContentRemovalJournalStorageTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalContentRemovalJournalStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalContentRemovalJournalStorageTest)

add_executable(CompanionSingleFileRemovalPlanTest CompanionSingleFileRemovalPlanTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionSingleFileRemovalPlanTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionSingleFileRemovalPlanTest)

add_executable(HalSingleFileRemovalPlanStorageTest HalSingleFileRemovalPlanStorageTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalSingleFileRemovalPlanStorageTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalSingleFileRemovalPlanStorageTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalSingleFileRemovalPlanStorageTest)

add_executable(CompanionRemovalMetadataSnapshotTest CompanionRemovalMetadataSnapshotTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionRemovalMetadataSnapshotTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionRemovalMetadataSnapshotTest)

add_executable(HalRemovalMetadataSnapshotStorageTest HalRemovalMetadataSnapshotStorageTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalRemovalMetadataSnapshotStorageTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalRemovalMetadataSnapshotStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalRemovalMetadataSnapshotStorageTest)

add_executable(HalCompletedContentRemovalsTest HalCompletedContentRemovalsTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalCompletedContentRemovalsTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalCompletedContentRemovalsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalCompletedContentRemovalsTest)

add_executable(HalContentRemovalTransactionsTest HalContentRemovalTransactionsTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalContentRemovalTransactionsTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalContentRemovalTransactionsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalContentRemovalTransactionsTest)

add_executable(CompanionContentRemovalHandlerTest CompanionContentRemovalHandlerTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionContentRemovalHandlerTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionContentRemovalHandlerTest)

add_executable(CompanionRemovalPathCollectionTest CompanionRemovalPathCollectionTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionRemovalPathCollectionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionRemovalPathCollectionTest)

add_executable(CompanionMultiPathRemovalPlanTest CompanionMultiPathRemovalPlanTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_compile_definitions(CompanionMultiPathRemovalPlanTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(CompanionMultiPathRemovalPlanTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionMultiPathRemovalPlanTest)

add_executable(HalRemovalStageClaimStorageTest HalRemovalStageClaimStorageTest.cpp ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalRemovalStageClaimStorageTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_compile_definitions(HalRemovalStageClaimStorageTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalRemovalStageClaimStorageTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalRemovalStageClaimStorageTest)

add_executable(TintaNativeClockTest TintaNativeClockTest.cpp ${REPO_ROOT}/src/activities/tinta/platform/Clock.cpp)
target_compile_definitions(TintaNativeClockTest PRIVATE LILA_TINTA=1)
target_include_directories(TintaNativeClockTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tinta_clock_stubs ${REPO_ROOT}/lib/Tinta/src ${REPO_ROOT}/lib/Companion)
target_link_libraries(TintaNativeClockTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TintaNativeClockTest)

add_executable(TintaNativeProfileMutationTest TintaNativeProfileMutationTest.cpp
  ${REPO_ROOT}/src/activities/tinta/platform/Clock.cpp
  ${REPO_ROOT}/lib/Companion/CompanionIdentity.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_compile_definitions(TintaNativeProfileMutationTest PRIVATE LILA_TINTA=1)
target_include_directories(TintaNativeProfileMutationTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tinta_clock_stubs ${REPO_ROOT}/lib/Tinta/src ${REPO_ROOT}/lib/Companion)
target_link_libraries(TintaNativeProfileMutationTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(TintaNativeProfileMutationTest)

add_executable(CompanionContentReadTest CompanionContentReadTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionContentReadTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionContentReadTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
gtest_discover_tests(CompanionContentReadTest)

add_executable(HalContentReadSourceTest HalContentReadSourceTest.cpp ../../lib/hal/HalInventoryFileHash.cpp)
target_include_directories(HalContentReadSourceTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalContentReadSourceTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalContentReadSourceTest)

add_executable(HalContentReadSessionTest HalContentReadSessionTest.cpp ../../lib/hal/HalInventoryFileHash.cpp ../../lib/Companion/CompanionRecords.cpp
  ../../lib/hal/HalInventoryIndexStorage.cpp ../../lib/hal/HalInventoryPublicationValidator.cpp)
target_include_directories(HalContentReadSessionTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${CMAKE_CURRENT_SOURCE_DIR}/hal_inventory_stubs ${REPO_ROOT}/lib/Companion ${REPO_ROOT}/lib/hal)
target_link_libraries(HalContentReadSessionTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalContentReadSessionTest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME FontInstallerFaultCheck
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/font_installer_fault_check.py
          --compiler ${CMAKE_CXX_COMPILER})

add_executable(CompanionCourseRemovalPlanTest CompanionCourseRemovalPlanTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionCourseRemovalPlanTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionCourseRemovalPlanTest)

add_executable(CompanionCourseRemovalParticipantTest CompanionCourseRemovalParticipantTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionCourseRemovalParticipantTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionCourseRemovalParticipantTest)

add_executable(HalCourseRemovalPlanStorageTest HalCourseRemovalPlanStorageTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalCourseRemovalPlanStorageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalCourseRemovalPlanStorageTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalCourseRemovalPlanStorageTest)

add_executable(CompanionCourseRemovalProofTest CompanionCourseRemovalProofTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionCourseRemovalProofTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionCourseRemovalProofTest)

add_executable(HalCourseRemovalProofStorageTest HalCourseRemovalProofStorageTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalCourseRemovalProofStorageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalCourseRemovalProofStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalCourseRemovalProofStorageTest)

add_executable(CompanionDictionaryRemovalPlanTest CompanionDictionaryRemovalPlanTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionDictionaryRemovalPlanTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryRemovalPlanTest)

add_executable(CompanionDictionaryRemovalParticipantTest CompanionDictionaryRemovalParticipantTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionDictionaryRemovalParticipantTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryRemovalParticipantTest)

add_executable(CompanionDictionaryRemovalCohortPlanTest CompanionDictionaryRemovalCohortPlanTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionDictionaryRemovalCohortPlanTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionDictionaryRemovalCohortPlanTest)

add_executable(HalDictionaryRemovalPlanAssemblyTest HalDictionaryRemovalPlanAssemblyTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp ${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp)
target_include_directories(HalDictionaryRemovalPlanAssemblyTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_compile_definitions(HalDictionaryRemovalPlanAssemblyTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(HalDictionaryRemovalPlanAssemblyTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalDictionaryRemovalPlanAssemblyTest)

add_executable(HalCourseRemovalAdmissionTest HalCourseRemovalAdmissionTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionRecords.cpp)
target_include_directories(HalCourseRemovalAdmissionTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/hal_transfer_stubs ${REPO_ROOT}/lib/Companion)
target_link_libraries(HalCourseRemovalAdmissionTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(HalCourseRemovalAdmissionTest)

add_executable(CompanionCourseContextTest CompanionCourseContextTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_compile_definitions(CompanionCourseContextTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures/")
target_link_libraries(CompanionCourseContextTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompanionCourseContextTest)

add_executable(CompanionCoursePackArchiveTest CompanionCoursePackArchiveTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionCoursePackArchiveTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(CompanionCoursePackArchiveTest)

add_executable(CompanionCourseBaselineImportTest CompanionCourseBaselineImportTest.cpp ../../lib/Companion/CompanionRecords.cpp)
target_link_libraries(CompanionCourseBaselineImportTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(CompanionCourseBaselineImportTest PRIVATE COURSE_BASELINE_IMPORT_FIXTURE="${REPO_ROOT}/protocol/fixtures/CourseBaselineImportRequest-v1.fixture" COURSE_BASELINE_REVIEW_FIXTURE="${REPO_ROOT}/protocol/fixtures/CourseBaselineReview-v1.fixture" COURSE_BASELINE_PUBLICATION_FIXTURE="${REPO_ROOT}/protocol/fixtures/CourseBaselinePublication-v1.fixture")
gtest_discover_tests(CompanionCourseBaselineImportTest)
