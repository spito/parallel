#include "thread_pool.h"
#include "timer.h"

#include <iostream>
#include <thread>

std::string timerState(parallel::Timer::State state);

int main() {

    using namespace std::literals;

    auto pool = parallel::ThreadPool::createSimple(2);

    parallel::Timer timer(*pool, 1000);

    auto h1 = timer.addDelayedTask(2s, []{
        std::cout << "Here1\n";
        std::this_thread::sleep_for(1s);
        std::cout << "and there1\n";
    });

    auto h2 = timer.addDelayedTask(2s, []{
        std::cout << "Here2\n";
        std::this_thread::sleep_for(1s);
        std::cout << "and there2\n";
    });

    while (true) {
        auto state1 = h1.getState();
        auto state2 = h2.getState();
        std::cout << "States: " << timerState(state1) << " " << timerState(state2) << std::endl;
        std::this_thread::sleep_for(100ms);

        if (state2 == parallel::Timer::State::WAITING)
            std::cout << "cancel: " << h2.cancel() << std::endl;

        if (state1 == parallel::Timer::State::FINISHED)
            break;
    }

    return 0;
}

std::string timerState(parallel::Timer::State state) {
    switch (state) {
        case parallel::Timer::State::WAITING:
            return "waiting";

        case parallel::Timer::State::BUSY:
            return "busy";

        case parallel::Timer::State::CANCELLED:
            return "cancelled";

        case parallel::Timer::State::EXCEPTION:
            return "exception";

        case parallel::Timer::State::FAILED_TO_SCHEDULE:
            return "fail to schedule";

        case parallel::Timer::State::FINISHED:
            return "finished";
    }
    return "////";
}

