#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "api_server.h"
#include "buffer_pool_manager.h"
#include "config.h"
#include "demo/load_generator.h"
#include "executor.h"
#include "logging.h"
#include "wal.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "crow.h"

using namespace relational_db;

namespace {

struct RuntimeStorage {
    std::shared_ptr<FileDiskManager> disk;
    std::shared_ptr<WriteAheadLog> wal;
    std::shared_ptr<BufferPoolManager> buffer_pool;
    std::shared_ptr<Executor> executor;
};

struct ShutdownState {
    std::mutex mutex;
    std::condition_variable changed;
    bool signal_received{false};
    bool server_finished{false};
    bool server_failed{false};
    std::string server_error;
    int signal_number{0};
};

class CrowJsonLogHandler final : public crow::ILogHandler {
   public:
    void log(std::string message, crow::LogLevel level) override {
        const char* level_name = "info";
        switch (level) {
            case crow::LogLevel::Debug:
                level_name = "debug";
                break;
            case crow::LogLevel::Info:
                level_name = "info";
                break;
            case crow::LogLevel::Warning:
                level_name = "warn";
                break;
            case crow::LogLevel::Error:
            case crow::LogLevel::Critical:
                level_name = "error";
                break;
        }
        LogMessage(level_name, "http_server", message);
    }
};

std::string ResolveDashboardPath() {
    if (std::filesystem::exists("dashboard/index.html")) {
        return "dashboard";
    }
    if (std::filesystem::exists("../dashboard/index.html")) {
        return "../dashboard";
    }
    return "dashboard";
}

DurabilityMode ParseDurability(const Config& config) {
    if (config.durability == "full") {
        return DurabilityMode::FULL;
    }
    if (config.durability == "normal") {
        return DurabilityMode::NORMAL;
    }
    return DurabilityMode::OFF;
}

RuntimeStorage OpenStorage(const Config& config) {
    RuntimeStorage storage;
    storage.disk = std::make_shared<FileDiskManager>(config.db_path, true);
    storage.wal = std::make_shared<WriteAheadLog>(config.db_path, config.wal_path,
                                                  ParseDurability(config),
                                                  config.wal_sync_interval_ms,
                                                  config.wal_max_bytes);
    (void)storage.wal->Recover(*storage.disk);
    storage.disk->ValidateHeader();
    storage.buffer_pool = std::make_shared<BufferPoolManager>(
        static_cast<size_t>(config.buffer_pool_frames), storage.disk, config.buffer_pool_policy);
    storage.executor = std::make_shared<Executor>(storage.buffer_pool, *storage.wal);
    return storage;
}

void PrintAdminResult(const std::string& json) {
    std::cout << json << '\n';
}

int RunHealthcheck(uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return 1;
    }
    struct WinsockCleanup {
        ~WinsockCleanup() {
            WSACleanup();
        }
    } winsock_cleanup;
    using SocketHandle = SOCKET;
    constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
    const auto close_socket = [](SocketHandle socket) { closesocket(socket); };
#else
    using SocketHandle = int;
    constexpr SocketHandle kInvalidSocket = -1;
    const auto close_socket = [](SocketHandle socket) { close(socket); };
#endif
    const SocketHandle socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle == kInvalidSocket) {
        return 1;
    }
    struct SocketCleanup {
        SocketHandle socket;
        decltype(close_socket)& close;
        ~SocketCleanup() {
            close(socket);
        }
    } socket_cleanup{socket_handle, close_socket};

#ifdef _WIN32
    const DWORD timeout = 1000;
    (void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    (void)setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{1, 0};
    (void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return 1;
    }

    constexpr char request[] = "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < sizeof(request) - 1U) {
#ifdef _WIN32
        const int result =
            send(socket_handle, request + sent, static_cast<int>(sizeof(request) - 1U - sent), 0);
#else
        const ssize_t result = send(socket_handle, request + sent, sizeof(request) - 1U - sent, 0);
#endif
        if (result <= 0) {
            return 1;
        }
        sent += static_cast<size_t>(result);
    }

    char response[128]{};
#ifdef _WIN32
    const int received = recv(socket_handle, response, static_cast<int>(sizeof(response) - 1U), 0);
#else
    const ssize_t received = recv(socket_handle, response, sizeof(response) - 1U, 0);
#endif
    if (received <= 0) {
        return 1;
    }
    const std::string status_line(response, static_cast<size_t>(received));
    return status_line.rfind("HTTP/1.1 200", 0) == 0 || status_line.rfind("HTTP/1.0 200", 0) == 0
               ? 0
               : 1;
}

#ifndef _WIN32
bool BlockShutdownSignals(sigset_t& signal_set) {
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);
    sigaddset(&signal_set, SIGUSR1);
    return pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) == 0;
}

void RunSignalWaiter(const sigset_t& signal_set, ShutdownState& state) {
    int received_signal = 0;
    if (sigwait(&signal_set, &received_signal) != 0 || received_signal == SIGUSR1) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.signal_number = received_signal;
        state.signal_received = true;
    }
    state.changed.notify_all();

    for (;;) {
        if (sigwait(&signal_set, &received_signal) != 0) {
            continue;
        }
        if (received_signal == SIGUSR1) {
            return;
        }
        _Exit(128 + received_signal);
    }
}
#else
std::atomic<int> g_windows_signal{0};

void HandleWindowsSignal(int signal_number) {
    if (g_windows_signal.exchange(signal_number, std::memory_order_relaxed) != 0) {
        _Exit(128 + signal_number);
    }
}

void RunSignalWaiter(ShutdownState& state) {
    while (true) {
        const int received_signal = g_windows_signal.exchange(0, std::memory_order_relaxed);
        if (received_signal != 0) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.signal_number = received_signal;
            state.signal_received = true;
            state.changed.notify_all();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
#endif

void SendSignalWaiterStop(std::thread& signal_thread) {
#ifndef _WIN32
    if (signal_thread.joinable()) {
        (void)pthread_kill(signal_thread.native_handle(), SIGUSR1);
    }
#else
    (void)signal_thread;
#endif
}

int RunAdministrativeCommand(const std::string& command, const Config& config) {
    try {
        RuntimeStorage storage = OpenStorage(config);
        if (command == "--check") {
            const std::optional<std::string> issue = storage.executor->IntegrityCheck();
            if (issue) {
                PrintAdminResult("{\"ok\":false,\"command\":\"check\",\"error\":\"" +
                                 EscapeLogJson(*issue) + "\"}");
                return 1;
            }
            PrintAdminResult("{\"ok\":true,\"command\":\"check\"}");
            return 0;
        }
        if (command == "--checkpoint") {
            storage.wal->Checkpoint(*storage.buffer_pool);
            storage.disk->Sync();
            PrintAdminResult("{\"ok\":true,\"command\":\"checkpoint\"}");
            return 0;
        }
    } catch (const std::exception& error) {
        LogMessage("error", "admin_command_failed", error.what());
        PrintAdminResult("{\"ok\":false,\"command\":\"" + command.substr(2) +
                         "\",\"error\":\"" + EscapeLogJson(error.what()) + "\"}");
        return 1;
    }
    return 64;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string command(argv[1]);
        if (command == "--version") {
            PrintAdminResult("{\"version\":\"" + std::string(CUSTOMRELATIONALDB_VERSION) +
                             "\",\"git_sha\":\"" + RELDB_GIT_SHA + "\"}");
            return 0;
        }
        if (command != "--check" && command != "--checkpoint" && command != "--healthcheck") {
            LogMessage("error", "invalid_command", "Expected --version, --check, --checkpoint, or --healthcheck");
            return 64;
        }
    }

#ifndef _WIN32
    sigset_t signal_set{};
    if (!BlockShutdownSignals(signal_set)) {
        LogMessage("error", "signal_mask_failed", "Unable to block SIGINT and SIGTERM");
        return 1;
    }
#else
    std::signal(SIGINT, HandleWindowsSignal);
    std::signal(SIGTERM, HandleWindowsSignal);
#endif

    Config config;
    try {
        config = Config::LoadFromEnvironment();
    } catch (const std::exception& error) {
        LogMessage("error", "configuration_invalid", error.what());
        return 78;
    }

    if (argc > 1) {
        const std::string command(argv[1]);
        if (command == "--healthcheck") {
            return RunHealthcheck(config.port);
        }
        return RunAdministrativeCommand(command, config);
    }

    const std::string dashboard_path = ResolveDashboardPath();
    RuntimeStorage storage;
    try {
        storage = OpenStorage(config);
    } catch (const std::exception& error) {
        LogMessage("error", "database_initialization_failed", error.what());
        return 1;
    }

    std::shared_ptr<ApiServer> api_server;
    try {
        api_server = std::make_shared<ApiServer>(storage.buffer_pool, storage.disk, storage.executor,
                                                 dashboard_path, config);
    } catch (const std::exception& error) {
        LogMessage("error", "api_server_configuration_failed", error.what());
        return 1;
    }

    LogJson("info", "effective_config", config.EffectiveJson());
    LogMessage("info", "service_starting", "Starting HTTP service");

    static CrowJsonLogHandler crow_log_handler;
    crow::logger::setHandler(&crow_log_handler);
    switch (config.log_level == "debug" ? 0 : config.log_level == "info" ? 1
                                          : config.log_level == "warn" ? 2
                                          : config.log_level == "error" ? 3
                                                                         : 5) {
        case 0:
            crow::logger::setLogLevel(crow::LogLevel::Debug);
            break;
        case 1:
            crow::logger::setLogLevel(crow::LogLevel::Info);
            break;
        case 2:
            crow::logger::setLogLevel(crow::LogLevel::Warning);
            break;
        case 3:
            crow::logger::setLogLevel(crow::LogLevel::Error);
            break;
        default:
            crow::logger::setLogLevel(crow::LogLevel::Critical);
            break;
    }

    std::unique_ptr<DemoLoadGenerator> demo_load;
    if (config.demo_load) {
        demo_load = std::make_unique<DemoLoadGenerator>(*storage.executor);
    }

    ShutdownState shutdown;
#ifndef _WIN32
    std::thread signal_thread([&signal_set, &shutdown]() { RunSignalWaiter(signal_set, shutdown); });
#else
    std::thread signal_thread([&shutdown]() { RunSignalWaiter(shutdown); });
#endif
    std::thread server_thread([&]() {
        try {
            api_server->Run(config.port);
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(shutdown.mutex);
            shutdown.server_failed = true;
            shutdown.server_error = error.what();
        }
        {
            std::lock_guard<std::mutex> lock(shutdown.mutex);
            shutdown.server_finished = true;
        }
        shutdown.changed.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(shutdown.mutex);
        shutdown.changed.wait(lock, [&shutdown]() {
            return shutdown.signal_received || shutdown.server_finished;
        });
        if (!shutdown.signal_received) {
            const std::string error = shutdown.server_error;
            lock.unlock();
            SendSignalWaiterStop(signal_thread);
            server_thread.join();
            if (signal_thread.joinable()) {
                signal_thread.join();
            }
            LogMessage("error", "http_server_stopped", error.empty()
                                                               ? "HTTP server stopped unexpectedly"
                                                               : error);
            return 1;
        }
    }

    LogMessage("info", "shutdown_signal_received", "Entering graceful shutdown");
    api_server->BeginDrain();
    demo_load.reset();
    const bool drained = api_server->WaitForDrain(
        std::chrono::milliseconds(config.drain_timeout_ms));
    LogJson(drained ? "info" : "error", "drain_complete",
            "{\"drained\":" + std::string(drained ? "true" : "false") +
                ",\"timeout_ms\":" + std::to_string(config.drain_timeout_ms) + "}");
    api_server->Stop();
    server_thread.join();

    int exit_code = 0;
    try {
        storage.wal->Checkpoint(*storage.buffer_pool);
        storage.disk->Sync();
        LogMessage("info", "shutdown_checkpoint_complete",
                   "Checkpoint and database/WAL sync completed");
    } catch (const std::exception& error) {
        LogMessage("error", "shutdown_checkpoint_failed", error.what());
        exit_code = 1;
    }
    if (!drained) {
        exit_code = 1;
    }
    SendSignalWaiterStop(signal_thread);
    if (signal_thread.joinable()) {
        signal_thread.join();
    }
    LogMessage(exit_code == 0 ? "info" : "error", "shutdown_complete",
               exit_code == 0 ? "Service stopped cleanly" : "Service stopped with errors");
    return exit_code;
}
