find_package(GTest REQUIRED)
include_directories(../src)

add_executable(p-thread-pool-test thread_pool.cpp)
target_link_libraries(p-thread-pool-test p-thread-pool ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES})

add_executable(p-state-test state.cpp)
target_link_libraries(p-state-test ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)

