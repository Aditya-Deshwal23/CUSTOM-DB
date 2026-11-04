#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "executor.h"
#include "wal.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t kMaxRows = 1'000'000;
constexpr uint64_t kMaxOperations = 1'000'000;

struct Options {
    uint64_t rows{10000};
    uint64_t operations{1000};
    uint64_t seed{1337};
    size_t threads{1};
    std::string workload{"point"};
    std::string durability{"full"};
};

struct Paths {
    explicit Paths(std::string prefix) {
        const auto stamp = Clock::now().time_since_epoch().count();
        const std::string stem =
            (std::filesystem::temp_directory_path() / (std::move(prefix) + std::to_string(stamp)))
                .string();
        engine = stem + ".custom";
        sqlite = stem + ".sqlite";
    }

    ~Paths() {
        for (const std::string& path : {engine, sqlite}) {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove(path + "-wal", error);
            std::filesystem::remove(path + "-shm", error);
            std::filesystem::remove(path + "-wal.tmp", error);
            std::filesystem::remove(path + ".ack", error);
        }
    }

    std::string engine;
    std::string sqlite;
};

struct EngineDatabase {
    EngineDatabase(const std::string& path, const Options& options)
        : database_path(path),
          disk(std::make_shared<relational_db::DiskManager>(path)),
          wal(std::make_unique<relational_db::WriteAheadLog>(path,
                                                             relational_db::DurabilityMode::OFF)),
          pool(std::make_shared<relational_db::BufferPoolManager>(4096, disk)) {
        wal->Recover(*disk);
        executor = std::make_unique<relational_db::Executor>(pool, *wal);
        executor->ExecuteQuery(
            "CREATE TABLE bench (id BIGINT PRIMARY KEY, value BIGINT, group_id BIGINT)");
        executor->ExecuteQuery("CREATE TABLE groups (id BIGINT PRIMARY KEY, value BIGINT)");
        const uint64_t group_count = std::min<uint64_t>(options.rows, 1000U);
        InsertGroups(group_count);
        InsertRows(options.rows, group_count);
        SetDurability(options.durability);
    }

    ~EngineDatabase() {
        executor.reset();
        pool.reset();
        wal.reset();
        disk.reset();
    }

    static relational_db::DurabilityMode ParseDurability(const std::string& value) {
        if (value == "full") {
            return relational_db::DurabilityMode::FULL;
        }
        if (value == "normal") {
            return relational_db::DurabilityMode::NORMAL;
        }
        return relational_db::DurabilityMode::OFF;
    }

    void SetDurability(const std::string& value) {
        if (value == "off") {
            return;
        }
        executor->ExecuteQuery("CHECKPOINT");
        executor.reset();
        pool.reset();
        wal.reset();
        wal = std::make_unique<relational_db::WriteAheadLog>(database_path, ParseDurability(value));
        wal->Recover(*disk);
        pool = std::make_shared<relational_db::BufferPoolManager>(4096, disk);
        executor = std::make_unique<relational_db::Executor>(pool, *wal);
    }

    void InsertGroups(uint64_t count) {
        constexpr uint64_t kBatch = 256;
        for (uint64_t first = 0; first < count; first += kBatch) {
            const uint64_t end = std::min(count, first + kBatch);
            std::string sql = "INSERT INTO groups VALUES ";
            for (uint64_t id = first; id < end; ++id) {
                if (id != first) {
                    sql.push_back(',');
                }
                sql += '(' + std::to_string(id) + ',' + std::to_string(id * 3U) + ')';
            }
            executor->ExecuteQuery(sql);
        }
    }

    void InsertRows(uint64_t count, uint64_t group_count) {
        constexpr uint64_t kBatch = 256;
        for (uint64_t first = 0; first < count; first += kBatch) {
            const uint64_t end = std::min(count, first + kBatch);
            std::string sql = "INSERT INTO bench VALUES ";
            for (uint64_t id = first; id < end; ++id) {
                if (id != first) {
                    sql.push_back(',');
                }
                sql += '(' + std::to_string(id) + ',' + std::to_string(id * 3U) + ',' +
                       std::to_string(id % group_count) + ')';
            }
            executor->ExecuteQuery(sql);
        }
    }

    std::string database_path;
    std::shared_ptr<relational_db::DiskManager> disk;
    std::unique_ptr<relational_db::WriteAheadLog> wal;
    std::shared_ptr<relational_db::BufferPoolManager> pool;
    std::unique_ptr<relational_db::Executor> executor;
};

void CheckSqlite(int result, sqlite3* db, const std::string& operation) {
    if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
        throw std::runtime_error(operation + ": " + sqlite3_errmsg(db));
    }
}

class SqliteConnection {
   public:
    SqliteConnection(const std::string& path, const std::string& durability,
                     bool configure_journal = false) {
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        const int result = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
        if (result != SQLITE_OK) {
            const std::string message =
                db_ == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(db_);
            if (db_ != nullptr) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error("opening SQLite database: " + message);
        }
        CheckSqlite(sqlite3_busy_timeout(db_, 10000), db_, "setting SQLite busy timeout");
        if (configure_journal) {
            Execute("PRAGMA journal_mode=WAL");
        }
        Execute("PRAGMA synchronous=" + (durability == "full"     ? std::string("FULL")
                                         : durability == "normal" ? std::string("NORMAL")
                                                                  : std::string("OFF")));
    }

    ~SqliteConnection() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    void Execute(const std::string& sql) {
        char* error = nullptr;
        const int result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
        if (result != SQLITE_OK) {
            const std::string message = error == nullptr ? sqlite3_errmsg(db_) : error;
            sqlite3_free(error);
            throw std::runtime_error("SQLite statement failed: " + message);
        }
    }

    sqlite3* Get() const noexcept {
        return db_;
    }

   private:
    sqlite3* db_{nullptr};
};

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement Prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    const int result = sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
    CheckSqlite(result, db, "preparing statement");
    return Statement(statement, sqlite3_finalize);
}

struct SqliteWorker {
    SqliteWorker(const std::string& path, const std::string& durability)
        : connection(path, durability),
          point(Prepare(connection.Get(), "SELECT value FROM bench WHERE id = ?1")),
          range(Prepare(connection.Get(),
                        "SELECT id FROM bench WHERE id >= ?1 AND id < ?2 ORDER BY id")),
          insert(Prepare(connection.Get(),
                         "INSERT INTO bench (id, value, group_id) VALUES (?1, ?2, ?3)")),
          update(Prepare(connection.Get(), "UPDATE bench SET value = value + 1 WHERE id = ?1")),
          join(Prepare(connection.Get(),
                       "SELECT b.id FROM bench b JOIN groups g ON b.group_id = g.id "
                       "WHERE b.id = ?1")) {}

    SqliteConnection connection;
    Statement point;
    Statement range;
    Statement insert;
    Statement update;
    Statement join;
};

void StepDone(sqlite3* db, sqlite3_stmt* statement) {
    const int result = sqlite3_step(statement);
    if (result != SQLITE_DONE) {
        CheckSqlite(result, db, "executing prepared statement");
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
}

uint64_t CountRows(sqlite3* db, sqlite3_stmt* statement) {
    uint64_t rows = 0;
    for (;;) {
        const int result = sqlite3_step(statement);
        if (result == SQLITE_DONE) {
            break;
        }
        CheckSqlite(result, db, "reading prepared query");
        ++rows;
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return rows;
}

void PopulateSqlite(const std::string& path, const Options& options) {
    SqliteConnection connection(path, "off", true);
    connection.Execute(
        "CREATE TABLE bench (id INTEGER PRIMARY KEY, value INTEGER, group_id INTEGER)");
    connection.Execute("CREATE TABLE groups (id INTEGER PRIMARY KEY, value INTEGER)");
    connection.Execute("BEGIN");
    Statement group_insert = Prepare(connection.Get(), "INSERT INTO groups VALUES (?1, ?2)");
    const uint64_t group_count = std::min<uint64_t>(options.rows, 1000U);
    for (uint64_t id = 0; id < group_count; ++id) {
        sqlite3_bind_int64(group_insert.get(), 1, static_cast<sqlite3_int64>(id));
        sqlite3_bind_int64(group_insert.get(), 2, static_cast<sqlite3_int64>(id * 3U));
        StepDone(connection.Get(), group_insert.get());
    }
    Statement row_insert = Prepare(connection.Get(), "INSERT INTO bench VALUES (?1, ?2, ?3)");
    for (uint64_t id = 0; id < options.rows; ++id) {
        sqlite3_bind_int64(row_insert.get(), 1, static_cast<sqlite3_int64>(id));
        sqlite3_bind_int64(row_insert.get(), 2, static_cast<sqlite3_int64>(id * 3U));
        sqlite3_bind_int64(row_insert.get(), 3, static_cast<sqlite3_int64>(id % group_count));
        StepDone(connection.Get(), row_insert.get());
    }
    connection.Execute("COMMIT");
}

bool IsWorkload(const std::string& workload) {
    return workload == "point" || workload == "range100" || workload == "insert" ||
           workload == "durable_insert" || workload == "update" || workload == "join" ||
           workload == "ycsb_a" || workload == "ycsb_b" || workload == "ycsb_c" ||
           workload == "ycsb_e";
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string flag(argv[index]);
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + flag);
        }
        const std::string value(argv[++index]);
        if (flag == "--rows") {
            options.rows = std::stoull(value);
        } else if (flag == "--operations") {
            options.operations = std::stoull(value);
        } else if (flag == "--threads") {
            options.threads = static_cast<size_t>(std::stoull(value));
        } else if (flag == "--seed") {
            options.seed = std::stoull(value);
        } else if (flag == "--workload") {
            options.workload = value;
        } else if (flag == "--durability") {
            options.durability = value;
        } else {
            throw std::invalid_argument("unknown option " + flag);
        }
    }
    if (options.rows < 100U || options.rows > kMaxRows || options.operations == 0U ||
        options.operations > kMaxOperations || options.threads == 0U || options.threads > 8U) {
        throw std::invalid_argument(
            "rows must be 100..1000000, operations 1..1000000, and threads 1..8");
    }
    if (!IsWorkload(options.workload)) {
        throw std::invalid_argument("unsupported workload: " + options.workload);
    }
    if (options.durability != "full" && options.durability != "normal" &&
        options.durability != "off") {
        throw std::invalid_argument("durability must be full, normal, or off");
    }
    if (options.workload == "durable_insert" && options.durability == "off") {
        throw std::invalid_argument("durable_insert requires full or normal durability");
    }
    return options;
}

uint64_t ExpectedOperations(uint64_t operations, size_t worker, size_t workers) {
    const uint64_t base = operations / workers;
    return base + (worker < operations % workers ? 1U : 0U);
}

std::string EngineSql(const std::string& kind, uint64_t key, uint64_t group_count) {
    if (kind == "point") {
        return "SELECT value FROM bench WHERE id = " + std::to_string(key);
    }
    if (kind == "range") {
        return "SELECT id FROM bench WHERE id >= " + std::to_string(key) + " AND id < " +
               std::to_string(key + 100U) + " ORDER BY id";
    }
    if (kind == "insert") {
        return "INSERT INTO bench VALUES (" + std::to_string(key) + ", " +
               std::to_string(key * 3U) + ", " + std::to_string(key % group_count) + ')';
    }
    if (kind == "update") {
        return "UPDATE bench SET value = value + 1 WHERE id = " + std::to_string(key);
    }
    return "SELECT b.id FROM bench b JOIN groups g ON b.group_id = g.id WHERE b.id = " +
           std::to_string(key);
}

void RunEngineOperation(relational_db::Executor& executor, const std::string& kind, uint64_t key,
                        uint64_t insert_id, uint64_t group_count) {
    const std::string sql = EngineSql(kind, kind == "insert" ? insert_id : key, group_count);
    const auto result = executor.ExecuteResult(sql, std::chrono::seconds(10), 1000U);
    if ((kind == "point" && result.rows.size() != 1U) ||
        (kind == "range" && result.rows.size() != 100U) ||
        (kind == "join" && result.rows.size() != 1U)) {
        throw std::runtime_error("CUSTOM-DB returned an unexpected row count for " + kind);
    }
}

void RunSqliteOperation(SqliteWorker& worker, const std::string& kind, uint64_t key,
                        uint64_t insert_id, uint64_t group_count) {
    sqlite3* const db = worker.connection.Get();
    if (kind == "point") {
        sqlite3_bind_int64(worker.point.get(), 1, static_cast<sqlite3_int64>(key));
        if (CountRows(db, worker.point.get()) != 1U) {
            throw std::runtime_error("SQLite returned an unexpected point-read row count");
        }
    } else if (kind == "range") {
        sqlite3_bind_int64(worker.range.get(), 1, static_cast<sqlite3_int64>(key));
        sqlite3_bind_int64(worker.range.get(), 2, static_cast<sqlite3_int64>(key + 100U));
        if (CountRows(db, worker.range.get()) != 100U) {
            throw std::runtime_error("SQLite returned an unexpected range-scan row count");
        }
    } else if (kind == "insert") {
        sqlite3_bind_int64(worker.insert.get(), 1, static_cast<sqlite3_int64>(insert_id));
        sqlite3_bind_int64(worker.insert.get(), 2, static_cast<sqlite3_int64>(insert_id * 3U));
        sqlite3_bind_int64(worker.insert.get(), 3,
                           static_cast<sqlite3_int64>(insert_id % group_count));
        StepDone(db, worker.insert.get());
    } else if (kind == "update") {
        sqlite3_bind_int64(worker.update.get(), 1, static_cast<sqlite3_int64>(key));
        StepDone(db, worker.update.get());
    } else {
        sqlite3_bind_int64(worker.join.get(), 1, static_cast<sqlite3_int64>(key));
        if (CountRows(db, worker.join.get()) != 1U) {
            throw std::runtime_error("SQLite returned an unexpected join row count");
        }
    }
}

std::string SelectOperation(const Options& options, std::mt19937_64& random) {
    if (options.workload == "point") {
        return "point";
    }
    if (options.workload == "range100") {
        return "range";
    }
    if (options.workload == "insert" || options.workload == "durable_insert") {
        return "insert";
    }
    if (options.workload == "update") {
        return "update";
    }
    if (options.workload == "join") {
        return "join";
    }
    const uint64_t draw = random() % 100U;
    if (options.workload == "ycsb_a") {
        return draw < 50U ? "point" : "update";
    }
    if (options.workload == "ycsb_b") {
        return draw < 95U ? "point" : "update";
    }
    if (options.workload == "ycsb_e") {
        return draw < 95U ? "range" : "insert";
    }
    return "point";
}

struct Result {
    double operations_per_second{0.0};
    double p50_us{0.0};
    double p95_us{0.0};
    double p99_us{0.0};
};

double Percentile(const std::vector<uint64_t>& samples, double quantile) {
    const size_t index = std::min(
        samples.size() - 1U,
        static_cast<size_t>(std::ceil(quantile * static_cast<double>(samples.size())) - 1.0));
    return static_cast<double>(samples[index]) / 1000.0;
}

const char* HostPlatform() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(_WIN32)
    return "Windows";
#else
    return "unknown";
#endif
}

const char* HostArchitecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

const char* CompilerVersion() {
#if defined(__VERSION__)
    return __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "unknown";
#endif
}

Result Measure(const Options& options, EngineDatabase& backend, bool engine,
               const std::string& sqlite_path) {
    const size_t workers_count = options.threads;
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::vector<uint64_t>> samples(workers_count);
    std::vector<std::string> errors(workers_count);
    std::vector<std::thread> workers;
    workers.reserve(workers_count);
    const uint64_t group_count = std::min<uint64_t>(options.rows, 1000U);
    for (size_t worker = 0; worker < workers_count; ++worker) {
        workers.emplace_back([&, worker]() {
            std::unique_ptr<SqliteWorker> sqlite_worker;
            try {
                if (!engine) {
                    sqlite_worker = std::make_unique<SqliteWorker>(sqlite_path, options.durability);
                }
                for (uint64_t warmup = 0; warmup < 100U; ++warmup) {
                    const uint64_t key =
                        (options.seed + worker * 17U + warmup * 7919U) % options.rows;
                    if (engine) {
                        RunEngineOperation(*backend.executor, "point", key, 0, group_count);
                    } else {
                        RunSqliteOperation(*sqlite_worker, "point", key, 0, group_count);
                    }
                }
            } catch (const std::exception& error) {
                errors[worker] = error.what();
            }
            ready.fetch_add(1U, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (errors[worker].empty()) {
                try {
                    std::mt19937_64 random(options.seed + worker);
                    auto& local_samples = samples[worker];
                    const uint64_t count =
                        ExpectedOperations(options.operations, worker, workers_count);
                    local_samples.reserve(static_cast<size_t>(count));
                    for (uint64_t operation = 0; operation < count; ++operation) {
                        const std::string kind = SelectOperation(options, random);
                        const uint64_t key = kind == "range" ? random() % (options.rows - 99U)
                                                             : random() % options.rows;
                        const uint64_t insert_id =
                            options.rows + worker * options.operations + operation;
                        const auto operation_start = Clock::now();
                        if (engine) {
                            RunEngineOperation(*backend.executor, kind, key, insert_id,
                                               group_count);
                        } else {
                            RunSqliteOperation(*sqlite_worker, kind, key, insert_id, group_count);
                        }
                        local_samples.push_back(static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                                 operation_start)
                                .count()));
                    }
                } catch (const std::exception& error) {
                    errors[worker] = error.what();
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != workers_count) {
        std::this_thread::yield();
    }
    const auto measured_start = Clock::now();
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - measured_start).count();
    for (const std::string& error : errors) {
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
    }

    std::vector<uint64_t> all_samples;
    all_samples.reserve(static_cast<size_t>(options.operations));
    for (auto& local : samples) {
        all_samples.insert(all_samples.end(), local.begin(), local.end());
    }
    std::sort(all_samples.begin(), all_samples.end());
    return {static_cast<double>(options.operations) / elapsed, Percentile(all_samples, 0.50),
            Percentile(all_samples, 0.95), Percentile(all_samples, 0.99)};
}

void PopulateSqliteAndEngine(const Paths& paths, const Options& options,
                             std::unique_ptr<EngineDatabase>& engine) {
    PopulateSqlite(paths.sqlite, options);
    engine = std::make_unique<EngineDatabase>(paths.engine, options);
}

int Run(const Options& options) {
    std::cerr << "fair benchmark seed=" << options.seed << " workload=" << options.workload
              << " threads=" << options.threads << '\n';
    Paths paths("custom-db-fair-bench-");
    std::unique_ptr<EngineDatabase> engine;
    PopulateSqliteAndEngine(paths, options, engine);

    const auto sqlite_result = Measure(options, *engine, false, paths.sqlite);
    const auto engine_result = Measure(options, *engine, true, paths.sqlite);
    const auto print_result = [](const char* name, const Result& result) {
        std::cout << "    \"" << name
                  << "\": {\"operations_per_second\": " << result.operations_per_second
                  << ", \"p50_us\": " << result.p50_us << ", \"p95_us\": " << result.p95_us
                  << ", \"p99_us\": " << result.p99_us << '}';
    };
    std::cout << "{\"workload\":\"" << options.workload << "\",\"rows\":" << options.rows
              << ",\"operations\":" << options.operations << ",\"threads\":" << options.threads
              << ",\"durability\":\"" << options.durability << "\",\"seed\":" << options.seed
              << ",\"host\":{\"platform\":\"" << HostPlatform() << "\",\"architecture\":\""
              << HostArchitecture() << "\",\"compiler\":\"" << CompilerVersion()
              << "\",\"sqlite_version\":\"" << sqlite3_libversion() << "\"}"
              << ",\"storage\":\"temporary file-backed databases; setup excluded\","
                 "\"cache\":\"warm filesystem cache and 100 seeded point-read warmups per thread\","
                 "\"sqlite_mode\":\"WAL with system libsqlite3 prepared statements\","
                 "\"custom_db_mode\":\"in-process Executor with matching SQL\","
                 "\"results\":{";
    print_result("custom_db_in_process", engine_result);
    std::cout << ',';
    print_result("sqlite_c_api_in_process", sqlite_result);
    std::cout << "}}\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Run(ParseOptions(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "fair benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
