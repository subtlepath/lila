# The accelerometer classifier behind Rotation: Auto, and its settle/once-per-turn bookkeeping.
add_executable(HeldOrientationTest HeldOrientationTest.cpp)

target_include_directories(HeldOrientationTest PRIVATE ${REPO_ROOT}/lib/hal)

target_link_libraries(HeldOrientationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HeldOrientationTest)
