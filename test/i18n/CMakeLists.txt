find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(OpenSSL REQUIRED)
file(GLOB I18N_TRANSLATIONS CONFIGURE_DEPENDS "${REPO_ROOT}/lib/I18n/translations/*.yaml")
function(add_language_tag_test target builtin source)
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}")
  file(MAKE_DIRECTORY "${generated}")
  configure_file("${REPO_ROOT}/lib/I18n/I18n.h" "${generated}/I18n.h" COPYONLY)
  configure_file("${REPO_ROOT}/lib/I18n/I18n.cpp" "${generated}/I18n.cpp" COPYONLY)
  add_custom_command(
    OUTPUT "${generated}/I18nKeys.h" "${generated}/I18nStrings.h" "${generated}/I18nStrings.cpp"
    COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/scripts/gen_i18n.py"
      "${REPO_ROOT}/lib/I18n/translations" "${generated}" --builtin-langs "${builtin}"
    DEPENDS "${REPO_ROOT}/scripts/gen_i18n.py" ${I18N_TRANSLATIONS}
    VERBATIM
  )
  add_executable(${target} ${source} "${generated}/I18n.cpp" "${generated}/I18nStrings.cpp")
  target_include_directories(${target} PRIVATE "${generated}")
  target_link_libraries(${target} PRIVATE GTest::gtest_main)
  gtest_discover_tests(${target})
endfunction()
add_language_tag_test(LanguageTagTest all LanguageTagTest.cpp)
add_language_tag_test(LanguageTagReducedTest EN LanguageTagReducedTest.cpp)

target_sources(LanguageTagTest PRIVATE NativeReaderPreferenceStoreTest.cpp NativeReaderPreferenceDependenciesTest.cpp)
target_include_directories(LanguageTagTest BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native_settings_stubs")
target_include_directories(LanguageTagTest PRIVATE "${REPO_ROOT}/src" "${REPO_ROOT}/lib/Companion" "${REPO_ROOT}/lib/EpdFont")

add_language_tag_test(NativeReaderPreferenceMetadataTest all NativeReaderPreferenceMetadataTest.cpp)
target_sources(NativeReaderPreferenceMetadataTest PRIVATE
  "${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp"
  "${REPO_ROOT}/lib/hal/HalInventoryFileHash.cpp"
  "${REPO_ROOT}/lib/hal/HalTintaJournalStorage.cpp"
  "${REPO_ROOT}/lib/hal/HalInventorySortStorage.cpp"
  "${REPO_ROOT}/lib/hal/HalInventoryIndexStorage.cpp"
  "${REPO_ROOT}/lib/Companion/CompanionRecords.cpp"
  "${REPO_ROOT}/lib/Companion/CompanionIdentity.cpp"
  "${REPO_ROOT}/lib/miniz/src/miniz_impl.c")
target_include_directories(NativeReaderPreferenceMetadataTest BEFORE PRIVATE
  "${REPO_ROOT}/test/companion/hal_transfer_stubs" "${CMAKE_CURRENT_SOURCE_DIR}/native_settings_stubs")
target_include_directories(NativeReaderPreferenceMetadataTest PRIVATE
  "${REPO_ROOT}" "${REPO_ROOT}/src" "${REPO_ROOT}/lib/Companion" "${REPO_ROOT}/lib/EpdFont"
  "${REPO_ROOT}/lib/hal" "${REPO_ROOT}/lib/miniz/src" "${REPO_ROOT}/lib/Memory")
target_compile_definitions(NativeReaderPreferenceMetadataTest PRIVATE COMPANION_FIXTURE_DIR="${REPO_ROOT}/protocol/fixtures")
target_link_libraries(NativeReaderPreferenceMetadataTest PRIVATE OpenSSL::Crypto)
