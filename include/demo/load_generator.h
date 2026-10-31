#ifndef RELDB_DEMO_LOAD_GENERATOR_H
#define RELDB_DEMO_LOAD_GENERATOR_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "executor.h"

namespace relational_db {

class DemoLoadGenerator {
   public:
    explicit DemoLoadGenerator(Executor& executor);
    ~DemoLoadGenerator();

    DemoLoadGenerator(const DemoLoadGenerator&) = delete;
    DemoLoadGenerator& operator=(const DemoLoadGenerator&) = delete;

   private:
    void Run();

    Executor& executor_;
    std::atomic<bool> stopping_{false};
    std::mutex wait_mutex_;
    std::condition_variable wait_condition_;
    std::thread worker_;
};

}  // namespace relational_db

#endif
