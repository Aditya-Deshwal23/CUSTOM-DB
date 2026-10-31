#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "api_server.h"
#include "b_plus_tree.h"
#include "buffer_pool_manager.h"
#include "executor.h"
#include "demo/load_generator.h"
#include "metrics/metrics.h"
#include "slotted_page.h"
#include "wal.h"

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

bool ReadDemoLoadSetting(bool& enabled) {
    const char* configured = std::getenv("DEMO_LOAD");
    enabled = configured != nullptr && std::string(configured) == "1";
    return configured == nullptr || std::string(configured) == "0" || enabled;
}

}  // namespace

int main() {
    std::signal(SIGINT, HandleShutdownSignal);
    std::signal(SIGTERM, HandleShutdownSignal);

    std::cerr << "Custom Relational Storage Engine Initialized (Native UCRT64 Mode)" << std::endl;

    const int port = ReadPortFromEnv();
    bool demo_load_enabled = false;
    if (!ReadDemoLoadSetting(demo_load_enabled)) {
        std::cerr << "DEMO_LOAD must be 0 or 1" << std::endl;
        return 2;
    }
    const std::string db_path = ReadDbPathFromEnv();
    const std::string dashboard_path = ResolveDashboardPath();

    std::cerr << "Selected PORT: " << port << std::endl;
    std::cerr << "Selected DB_PATH: " << db_path << std::endl;
    std::cerr << "Dashboard static path: " << dashboard_path << std::endl;
    std::cerr << "Demo load: " << (demo_load_enabled ? "enabled" : "disabled") << std::endl;

    std::shared_ptr<DiskManager> disk_manager;
    std::shared_ptr<WriteAheadLog> wal;
    try {
        disk_manager = std::make_shared<DiskManager>(db_path, true);
        wal = std::make_shared<WriteAheadLog>(db_path);
        wal->Recover(*disk_manager);
        disk_manager->ValidateHeader();
    } catch (const std::exception& ex) {
        std::cerr << "Failed to initialize database at '" << db_path << "': " << ex.what()
                  << std::endl;
        return 1;
    }
    auto bpm = std::make_shared<BufferPoolManager>(disk_manager);
    std::shared_ptr<Executor> executor;
    try {
        executor = std::make_shared<Executor>(bpm, *wal);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to initialize durable executor: " << ex.what() << std::endl;
        return 1;
    }
    std::shared_ptr<ApiServer> api_server;
    try {
        api_server = std::make_shared<ApiServer>(bpm, disk_manager, executor, dashboard_path);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to configure API server: " << ex.what() << std::endl;
        return 1;
    }

    std::atomic<bool> running{true};
    std::unique_ptr<DemoLoadGenerator> demo_load;
    if (demo_load_enabled) {
        demo_load = std::make_unique<DemoLoadGenerator>(*executor);
    }

    std::thread signal_thread([api_server, &running]() {
        while (running.load() && g_shutdown_requested == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (g_shutdown_requested != 0) {
            std::cerr << "Shutdown signal received, stopping server..." << std::endl;
            running.store(false);
            api_server->Stop();
        }
    });

    std::cerr << "Starting Web Dashboard at http://0.0.0.0:" << port << std::endl;
    api_server->Run(port);

    running.store(false);
    demo_load.reset();

    if (signal_thread.joinable()) {
        signal_thread.join();
    }

    try {
        wal->Checkpoint(*bpm);
    } catch (const std::exception& ex) {
        std::cerr << "Database checkpoint failed during shutdown: " << ex.what() << std::endl;
        return 1;
    }

    std::cerr << "Shutdown complete." << std::endl;
    return 0;
}
