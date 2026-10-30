#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <csignal>
#include "buffer_pool_manager.h"
#include "slotted_page.h"
#include "b_plus_tree.h"
#include "executor.h"
#include "api_server.h"

using namespace relational_db;

namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

void HandleShutdownSignal(int) {
    g_shutdown_requested = 1;
}

int ReadPortFromEnv() {
    const char* port_env = std::getenv("PORT");
    if (port_env == nullptr || std::string(port_env).empty()) {
        return 8080;
    }

    try {
        int port = std::stoi(port_env);
        if (port > 0 && port <= 65535) {
            return port;
        }
    } catch (const std::exception&) {
    }

    std::cerr << "Invalid PORT='" << port_env << "', defaulting to 8080" << std::endl;
    return 8080;
}

std::string ReadDbPathFromEnv() {
    const char* db_path_env = std::getenv("DB_PATH");
    if (db_path_env == nullptr || std::string(db_path_env).empty()) {
        return "./test.db";
    }
    return db_path_env;
}

std::string ResolveDashboardPath() {
    if (std::filesystem::exists("dashboard/index.html")) {
        return "dashboard";
    }
    if (std::filesystem::exists("../dashboard/index.html")) {
        return "../dashboard";
    }
    return "dashboard";
}

} // namespace

int main() {
    std::signal(SIGINT, HandleShutdownSignal);
    std::signal(SIGTERM, HandleShutdownSignal);

    std::cout << "Custom Relational Storage Engine Initialized (Native UCRT64 Mode)" << std::endl;

    const int port = ReadPortFromEnv();
    const std::string db_path = ReadDbPathFromEnv();
    const std::string dashboard_path = ResolveDashboardPath();

    std::cout << "Selected PORT: " << port << std::endl;
    std::cout << "Selected DB_PATH: " << db_path << std::endl;
    std::cout << "Dashboard static path: " << dashboard_path << std::endl;

    std::shared_ptr<DiskManager> disk_manager;
    try {
        disk_manager = std::make_shared<DiskManager>(db_path);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to initialize database at '" << db_path << "': " << ex.what()
                  << std::endl;
        return 1;
    }
    auto bpm = std::make_shared<BufferPoolManager>(disk_manager);
    auto executor = std::make_shared<Executor>(bpm);
    auto api_server = std::make_shared<ApiServer>(bpm, disk_manager, executor, dashboard_path);

    std::atomic<bool> running{true};

    std::thread signal_thread([api_server, &running]() {
        while (running.load() && g_shutdown_requested == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (g_shutdown_requested != 0) {
            std::cout << "Shutdown signal received, stopping server..." << std::endl;
            running.store(false);
            api_server->Stop();
        }
    });

    std::cout << "Starting Web Dashboard at http://0.0.0.0:" << port << std::endl;
    api_server->Run(port);

    running.store(false);

    if (signal_thread.joinable()) {
        signal_thread.join();
    }

    std::cout << "Shutdown complete." << std::endl;
    return 0;
}
