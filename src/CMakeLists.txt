

add_library(p-thread-pool thread_pool.cpp)
target_link_libraries(p-thread-pool pthread)

add_library(p-timer timer.cpp)
target_link_libraries(p-timer p-thread_pool)