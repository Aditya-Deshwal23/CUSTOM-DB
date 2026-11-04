#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "internal.h"
#include "metrics/metrics.h"

namespace relational_db {

ApiServer::Impl::Impl(std::shared_ptr<BufferPoolManager> buffer_pool,
                      std::shared_ptr<IDiskManager> disk, std::shared_ptr<Executor> query_executor,
                      std::string static_path, Config service_config)
    : bpm(std::move(buffer_pool)),
      disk_manager(std::move(disk)),
      executor(std::move(query_executor)),
      config(std::move(service_config)) {
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

    for (const char* file_name :
         {"index.html", "index.css", "index.js", "js/filter.js", "js/exporters.js",
          "js/er-layout.js", "js/grid.js", "js/studio.js"}) {
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
    StartMetricsSampler();
    recovering.store(false, std::memory_order_release);
}

ApiServer::Impl::~Impl() {
    stop_sampler.store(true, std::memory_order_release);
    history_changed.notify_all();
    if (sampler_thread.joinable()) {
        sampler_thread.join();
    }
}

void ApiServer::Impl::StartMetricsSampler() {
    sampler_thread = std::thread([this]() {
        while (!stop_sampler.load(std::memory_order_acquire)) {
            SampleMetrics();
            std::unique_lock<std::mutex> lock(history_mutex);
            history_changed.wait_for(lock, std::chrono::seconds(1), [this]() {
                return stop_sampler.load(std::memory_order_acquire);
            });
        }
    });
}

void ApiServer::Impl::SampleMetrics() {
    const BufferPoolStats pool = bpm->SnapshotStats();
    const DiskStats disk = disk_manager->Stats();
    const MetricsRegistry& metrics = MetricsRegistry::Instance();
    const ApiMiddleware& middleware = app.get_middleware<ApiMiddleware>();
    ApiHistorySample sample;
    sample.ts_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
    size_t index = 0;
    const auto add = [&sample, &index](uint64_t value) { sample.values[index++] = value; };
    add(pool.hits);
    add(pool.misses);
    add(pool.evictions);
    add(pool.dirty_write_backs);
    add(disk.reads);
    add(disk.writes);
    add(disk.syncs);
    add(disk.bytes);
    add(metrics.rows_read.Value());
    add(metrics.rows_written.Value());
    add(metrics.ErrorCount());
    add(middleware.requests.load(std::memory_order_relaxed));
    add(middleware.queries.load(std::memory_order_relaxed));
    add(middleware.active_requests.load(std::memory_order_relaxed));
    add(metrics.active_statements.Value());
    add(metrics.statement_waiters.Value());
    add(metrics.index_scans.Value());
    add(metrics.sequential_scans.Value());
    add(metrics.rate_limited_requests.Value());
    add(metrics.inspect_rate_limited_requests.Value());
    add(metrics.busy_rejections.Value());
    add(metrics.wal_bytes.Value());
    add(metrics.wal_commits.Value());
    add(metrics.checkpoints.Value());
    add(metrics.recovery_records.Value());
    for (const Histogram* histogram :
         {&metrics.disk_read_us, &metrics.disk_write_us, &metrics.disk_sync_us}) {
        const HistogramSnapshot snapshot = histogram->Snapshot();
        add(snapshot.count);
        add(snapshot.sum);
    }
    for (const Histogram& histogram : metrics.statement_duration_us) {
        const HistogramSnapshot snapshot = histogram.Snapshot();
        add(snapshot.count);
        add(snapshot.sum);
    }
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        history.push_back(std::move(sample));
        while (history.size() > 3600U) {
            history.pop_front();
        }
    }
}

ApiServer::ApiServer(std::shared_ptr<BufferPoolManager> bpm,
                     std::shared_ptr<IDiskManager> disk_manager, std::shared_ptr<Executor> executor,
                     std::string dashboard_path, Config config)
    : impl_(std::make_unique<Impl>(std::move(bpm), std::move(disk_manager), std::move(executor),
                                   std::move(dashboard_path), std::move(config))) {}

ApiServer::~ApiServer() = default;

void ApiServer::Run(int port) {
    impl_->app.server_name("").bindaddr("0.0.0.0").port(port).multithreaded().run();
}

void ApiServer::BeginDrain() {
    impl_->draining.store(true, std::memory_order_release);
    impl_->app.get_middleware<ApiMiddleware>().draining.store(true, std::memory_order_release);
}

bool ApiServer::WaitForDrain(std::chrono::milliseconds timeout) {
    return impl_->app.get_middleware<ApiMiddleware>().WaitForIdle(timeout);
}

void ApiServer::Stop() {
    impl_->draining.store(true, std::memory_order_release);
    impl_->app.stop();
}

}  // namespace relational_db
