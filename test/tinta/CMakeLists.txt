# Tinta's engine (lib/Tinta/src/core): its own minimal checks (check.h), one
# executable per *_test.cpp, linked with the core modules named on its
# "// modules:" line. Each also runs with the course pack read through the SD
# card's block cache (TINTA_PACK_SOURCE=file). run.sh does the same without
# CMake.
set(TINTA_ROOT ${REPO_ROOT}/lib/Tinta/src)
file(GLOB TINTA_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)

foreach(test_source ${TINTA_TESTS})
  get_filename_component(test_name ${test_source} NAME_WE)
  file(STRINGS ${test_source} modules_line REGEX "^// modules:" LIMIT_COUNT 1)
  string(REPLACE "// modules:" "" modules "${modules_line}")
  separate_arguments(modules)
  set(sources ${test_source})
  foreach(module ${modules})
    file(GLOB module_sources ${TINTA_ROOT}/core/${module}/*.cpp)
    list(APPEND sources ${module_sources})
  endforeach()

  set(target tinta_${test_name})
  add_executable(${target} ${sources})
  set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
  target_include_directories(${target} PRIVATE
    ${TINTA_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  )
  # -O1 as run.sh: Apple clang 21 at -O2 drops DayQueue::restore()'s loop
  # (UBSan finds nothing; the firmware's GCC keeps it).
  target_compile_options(${target} PRIVATE -O1 -Wall -Wextra -fno-exceptions)
  add_test(NAME ${target} COMMAND ${target} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${target}_sd COMMAND ${target} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(${target}_sd PROPERTIES ENVIRONMENT "TINTA_PACK_SOURCE=file")
endforeach()
