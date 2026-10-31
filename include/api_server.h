#ifndef API_SERVER_H
#define API_SERVER_H

#include <memory>
#include <chrono>
#include <string>

#include "buffer_pool_manager.h"
#include "config.h"
#include "disk_manager.h"
#include "executor.h"

namespace relational_db {

class ApiServer {
   public:
    struct Impl;

    ApiServer(std::shared_ptr<BufferPoolManager> bpm, std::shared_ptr<IDiskManager> disk_manager,
              std::shared_ptr<Executor> executor, std::string dashboard_path, Config config);
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    void Run(int port = 8080);
    void BeginDrain();
    bool WaitForDrain(std::chrono::milliseconds timeout);
    void Stop();

   private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace relational_db

#endif
