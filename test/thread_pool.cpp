#include "thread_pool.h"

#include <atomic>
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>


TEST(ThreadPool, Simple) {
    auto pool = parallel::ThreadPool::createSimple(1);

    bool flag = false;
    std::mutex m;
    std::condition_variable cv;

    pool->addTask([&] {
        {
            std::lock_guard lock(m);
            flag = true;
        }
        cv.notify_one();
    });

    std::unique_lock lock(m);
    cv.wait(lock, [&] { return flag; });
}

TEST(ThreadPool, Exception) {
    using namespace std::chrono_literals;

    std::atomic<bool> exceptionFired = false;
    auto pool = parallel::ThreadPool::createSimple(1, [&](std::exception_ptr ptr) {
        try {
            std::rethrow_exception(ptr);
        } catch (int) {
            exceptionFired = true;
        }
    });

    pool->addTask([]{ throw 2; });

    std::this_thread::sleep_for(1s);
    EXPECT_TRUE(exceptionFired);
}

TEST(ThreadPool, ExhaustCapacity) {
    using namespace std::chrono_literals;

    std::atomic<int> countPre = 0;
    std::atomic<int> countPost = 0;
    std::mutex m;
    std::condition_variable cv;

    auto task = [&] {
        ++countPre;
        std::unique_lock lock(m);
        cv.wait(lock);
        ++countPost;
    };

    auto pool = parallel::ThreadPool::createSimple(2);

    pool->addTask(task);
    pool->addTask(task);
    pool->addTask(task);

    std::this_thread::sleep_for(1s);
    EXPECT_EQ(countPre, 2);
    EXPECT_EQ(countPost, 0);

    cv.notify_one();
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(countPre, 3);
    EXPECT_EQ(countPost, 1);

    std::this_thread::sleep_for(1s);
    cv.notify_all();
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(countPre, 3);
    EXPECT_EQ(countPost, 3);
}