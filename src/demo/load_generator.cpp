#include "demo/load_generator.h"

#include <chrono>
#include <iostream>
#include <string>

#include "metrics/metrics.h"

namespace relational_db {

DemoLoadGenerator::DemoLoadGenerator(Executor& executor)
    : executor_(executor) {
    MetricsRegistry::Instance().demo_load_active.Set(1);
    worker_ = std::thread(&DemoLoadGenerator::Run, this);
}

DemoLoadGenerator::~DemoLoadGenerator() {
    stopping_.store(true, std::memory_order_release);
    wait_condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    MetricsRegistry::Instance().demo_load_active.Set(0);
}

void DemoLoadGenerator::Run() {
    uint64_t sequence = 0;
    while (!stopping_.load(std::memory_order_acquire)) {
        const int64_t key = static_cast<int64_t>(sequence % 10000U);
        try {
            (void)executor_.ExecuteQuery("SELECT " + std::to_string(key));
        } catch (const std::exception& error) {
            std::cerr << "DEMO_LOAD statement failed: " << error.what() << '\n';
        }
        ++sequence;
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_condition_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
            return stopping_.load(std::memory_order_acquire);
        });
    }
}

}  // namespace relational_db
