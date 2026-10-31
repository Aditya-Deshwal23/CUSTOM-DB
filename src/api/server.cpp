#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "internal.h"

namespace relational_db {

ApiServer::Impl::Impl(std::shared_ptr<BufferPoolManager> buffer_pool,
                      std::shared_ptr<IDiskManager> disk, std::shared_ptr<Executor> query_executor,
                      std::string static_path)
    : bpm(std::move(buffer_pool)),
      disk_manager(std::move(disk)),
      executor(std::move(query_executor)),
      config(LoadApiConfig()) {
    if (!bpm || !disk_manager || !executor) {
        throw std::invalid_argument("ApiServer dependencies must not be null");
    }

    recovering.store(true, std::memory_order_release);
    app.get_middleware<ApiMiddleware>().config = config;

    if (config.max_db_bytes != 0U) {
        const auto file_manager = std::dynamic_pointer_cast<FileDiskManager>(disk_manager);
        if (!file_manager) {
            throw std::invalid_argument("MAX_DB_BYTES requires FileDiskManager");
        }
        file_manager->SetMaxBytes(config.max_db_bytes);
    }

    for (const char* file_name : {"index.html", "index.css", "index.js"}) {
        const std::string file(file_name);
        std::ifstream input(static_path + "/" + file, std::ios::binary);
        if (input) {
            static_files.emplace(file, std::string(std::istreambuf_iterator<char>(input),
                                                   std::istreambuf_iterator<char>()));
            if (input.bad()) {
                throw std::runtime_error("Failed to load dashboard asset " + file);
            }
        }
    }
    RegisterRoutes();
    recovering.store(false, std::memory_order_release);
}

ApiServer::ApiServer(std::shared_ptr<BufferPoolManager> bpm,
                     std::shared_ptr<IDiskManager> disk_manager, std::shared_ptr<Executor> executor,
                     std::string dashboard_path)
    : impl_(std::make_unique<Impl>(std::move(bpm), std::move(disk_manager), std::move(executor),
                                   std::move(dashboard_path))) {}

ApiServer::~ApiServer() = default;

void ApiServer::Run(int port) {
    impl_->app.bindaddr("0.0.0.0").port(port).multithreaded().run();
}

void ApiServer::Stop() {
    impl_->draining.store(true, std::memory_order_release);
    impl_->app.stop();
}

}  // namespace relational_db
