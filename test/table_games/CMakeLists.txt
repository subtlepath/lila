add_executable(TableGamesTest
  TableGamesTest.cpp
  ${REPO_ROOT}/lib/TableGames/TableGame.cpp
  ${REPO_ROOT}/lib/TableGames/TableSession.cpp
  ${REPO_ROOT}/lib/TableGames/ConnectFour.cpp
  ${REPO_ROOT}/lib/TableGames/DotsAndBoxes.cpp
  ${REPO_ROOT}/lib/TableGames/LiarsDice.cpp
  ${REPO_ROOT}/lib/TableGames/MurderMystery.cpp
)

target_include_directories(TableGamesTest PRIVATE
  ${REPO_ROOT}/lib/TableGames
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(TableGamesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TableGamesTest)
