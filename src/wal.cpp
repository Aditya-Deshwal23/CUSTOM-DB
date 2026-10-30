#include "wal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <unordered_map>

#include "buffer_pool_manager.h"
#include "crc32c.h"
#include "db_error.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace relational_db {
namespace {

constexpr uint32_t kWalVersion = 1;
constexpr std::array<uint8_t, 8> kWalMagic{{'R', 'D', 'B', 'W', 'A', 'L', 0, 1}};

struct WalHeaderLayout {
    std::array<uint8_t, 8> magic;
    std::array<uint8_t, 4> version;
    std::array<uint8_t, 8> base_lsn;
    std::array<uint8_t, 4> crc32c;
};

struct WalRecordFixedLayout {
    std::array<uint8_t, 4> length;
    uint8_t type;
    std::array<uint8_t, 8> txn_id;
    std::array<uint8_t, 8> lsn;
    std::array<uint8_t, 4> crc32c;
};

struct WalPageImagePayloadLayout {
    std::array<uint8_t, 4> page_id;
    std::array<uint8_t, PAGE_SIZE> page_image;
};

static_assert(sizeof(WalHeaderLayout) == 24, "WAL header layout must be 24 bytes");
static_assert(offsetof(WalHeaderLayout, base_lsn) == 12, "WAL base LSN offset changed");
static_assert(offsetof(WalHeaderLayout, crc32c) == 20, "WAL header CRC offset changed");
static_assert(sizeof(WalRecordFixedLayout) == 25, "WAL fixed record layout must be 25 bytes");
static_assert(offsetof(WalRecordFixedLayout, txn_id) == 5, "WAL transaction ID offset changed");
static_assert(offsetof(WalRecordFixedLayout, lsn) == 13, "WAL LSN offset changed");
static_assert(offsetof(WalRecordFixedLayout, crc32c) == 21, "WAL record CRC offset changed");
static_assert(sizeof(WalPageImagePayloadLayout) == 4 + PAGE_SIZE,
              "WAL page-image payload size changed");

constexpr size_t kWalHeaderSize = sizeof(WalHeaderLayout);
constexpr size_t kRecordFixedSize = sizeof(WalRecordFixedLayout);

uint32_t GetU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
}

uint64_t GetU64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

void PutU16(uint8_t* bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void PutU32(uint8_t* bytes, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

void PutU64(uint8_t* bytes, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

std::string IoMessage(const std::string& action) {
    return action + ": " + std::strerror(errno);
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + 4U);
    PutU32(bytes.data() + offset, value);
}

void AppendU64(std::vector<uint8_t>& bytes, uint64_t value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + 8U);
    PutU64(bytes.data() + offset, value);
}

void WriteFullyAt(int descriptor, uint64_t offset, const uint8_t* buffer, size_t length) {
    size_t transferred = 0;
    while (transferred < length) {
#ifdef _WIN32
        if (_lseeki64(descriptor, static_cast<__int64>(offset + transferred), SEEK_SET) < 0) {
            throw DbError(ErrorCode::IO_ERROR, IoMessage("seek WAL"));
        }
        const unsigned int remaining = static_cast<unsigned int>(std::min(
            length - transferred, static_cast<size_t>(std::numeric_limits<unsigned int>::max())));
        const int amount = _write(descriptor, buffer + transferred, remaining);
#else
        const ssize_t amount = pwrite(descriptor, buffer + transferred, length - transferred,
                                      static_cast<off_t>(offset + transferred));
#endif
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            throw DbError(ErrorCode::IO_ERROR, IoMessage("write WAL"));
        }
        transferred += static_cast<size_t>(amount);
    }
}

bool ReadFullyAt(int descriptor, uint64_t offset, uint8_t* buffer, size_t length) {
    size_t transferred = 0;
    while (transferred < length) {
#ifdef _WIN32
        if (_lseeki64(descriptor, static_cast<__int64>(offset + transferred), SEEK_SET) < 0) {
            throw DbError(ErrorCode::IO_ERROR, IoMessage("seek WAL"));
        }
        const unsigned int remaining = static_cast<unsigned int>(std::min(
            length - transferred, static_cast<size_t>(std::numeric_limits<unsigned int>::max())));
        const int amount = _read(descriptor, buffer + transferred, remaining);
#else
        const ssize_t amount = pread(descriptor, buffer + transferred, length - transferred,
                                     static_cast<off_t>(offset + transferred));
#endif
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            return false;
        }
        transferred += static_cast<size_t>(amount);
    }
    return true;
}

uint64_t GetFileSize(int descriptor) {
#ifdef _WIN32
    const __int64 size = _filelengthi64(descriptor);
    if (size < 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("stat WAL"));
    }
    return static_cast<uint64_t>(size);
#else
    struct stat status{};
    if (fstat(descriptor, &status) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("stat WAL"));
    }
    return static_cast<uint64_t>(status.st_size);
#endif
}

void SyncDescriptor(int descriptor) {
#ifdef _WIN32
    if (_commit(descriptor) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("commit WAL"));
    }
#elif defined(__APPLE__)
    if (fsync(descriptor) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("fsync WAL"));
    }
#else
    if (fdatasync(descriptor) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("fdatasync WAL"));
    }
#endif
}

void SyncParentDirectory(const std::string& path) {
#ifdef _WIN32
    (void)path;
#else
    const size_t separator = path.find_last_of('/');
    const std::string directory =
        separator == std::string::npos ? "." : (separator == 0 ? "/" : path.substr(0, separator));
    const int directory_fd = open(directory.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                                         | O_DIRECTORY
#endif
    );
    if (directory_fd < 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("open WAL parent directory"));
    }
    const int result = fsync(directory_fd);
    const int sync_error = errno;
    close(directory_fd);
    if (result != 0) {
        errno = sync_error;
        throw DbError(ErrorCode::IO_ERROR, IoMessage("fsync WAL parent directory"));
    }
#endif
}

std::array<uint8_t, kWalHeaderSize> MakeHeader(uint64_t base_lsn) {
    std::array<uint8_t, kWalHeaderSize> header{};
    std::copy(kWalMagic.begin(), kWalMagic.end(), header.begin());
    PutU32(header.data() + 8, kWalVersion);
    PutU64(header.data() + 12, base_lsn);
    PutU32(header.data() + 20, Crc32c(header.data(), 20));
    return header;
}

std::array<uint8_t, PAGE_SIZE> MakePageImage(const WriteAheadLog::CommitPage& page, uint64_t lsn) {
    std::array<uint8_t, PAGE_SIZE> image{};
    PutU16(image.data() + 4, static_cast<uint16_t>(page.type));
    PutU16(image.data() + 6, page.flags);
    PutU64(image.data() + 8, lsn);
    std::memcpy(image.data() + PAGE_HEADER_SIZE, page.body.data(), PAGE_DATA_SIZE);
    if (page.page_id == 0) {
        uint8_t* body = image.data() + PAGE_HEADER_SIZE;
        constexpr size_t kHeaderCrcBodyOffset = 32;
        PutU32(body + kHeaderCrcBodyOffset, Crc32c(body, kHeaderCrcBodyOffset));
    }
    PutU32(image.data(), Crc32c(image.data() + 4, PAGE_SIZE - 4U));
    return image;
}

struct ScannedRecord {
    LogRecordType type;
    uint64_t txn_id;
    uint64_t lsn;
    std::vector<uint8_t> payload;
    uint64_t end_offset;
};

DurabilityMode ReadModeFromEnvironment() {
    const char* configured = std::getenv("DURABILITY");
    const std::string mode = configured == nullptr ? "full" : configured;
    if (mode == "full") {
        return DurabilityMode::FULL;
    }
    if (mode == "normal") {
        return DurabilityMode::NORMAL;
    }
    if (mode == "off") {
        const char* unsafe = std::getenv("ALLOW_UNSAFE");
        if (unsafe == nullptr || std::string(unsafe) != "1") {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "DURABILITY=off requires ALLOW_UNSAFE=1");
        }
        return DurabilityMode::OFF;
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT, "DURABILITY must be full, normal, or off");
}

uint64_t ReadUnsignedEnvironment(const char* name, uint64_t fallback) {
    const char* configured = std::getenv(name);
    if (configured == nullptr || *configured == '\0') {
        return fallback;
    }
    const char* end = configured + std::strlen(configured);
    uint64_t value = 0;
    const auto parsed = std::from_chars(configured, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      std::string(name) + " must be a positive integer");
    }
    return value;
}

}  // namespace

TxnContext::TxnContext(BufferPoolManager& buffer_pool, WriteAheadLog& wal, uint64_t txn_id,
                       uint64_t page_count_before)
    : buffer_pool_(&buffer_pool),
      wal_(&wal),
      txn_id_(txn_id),
      page_count_before_(page_count_before) {}

TxnContext::~TxnContext() {
    Rollback();
}

void TxnContext::Commit() {
    if (completed_) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Transaction has already completed");
    }
    try {
        buffer_pool_->CommitTransaction(*this);
        completed_ = true;
    } catch (...) {
        Rollback();
        throw;
    }
    if (wal_->ShouldCheckpoint()) {
        wal_->Checkpoint(*buffer_pool_);
    }
}

void TxnContext::Rollback() noexcept {
    if (completed_) {
        return;
    }
    buffer_pool_->RollbackTransaction(*this);
    completed_ = true;
}

void FailPoint::Hit(const std::string& name) {
    const char* configured = std::getenv("RELDB_FAILPOINT");
    if (configured == nullptr) {
        return;
    }
    const std::string requested(configured);
    const size_t separator = requested.find(':');
    if (separator == std::string::npos || requested.substr(0, separator) != name) {
        return;
    }
    const std::string action = requested.substr(separator + 1U);
    if (action == "kill") {
#ifdef _WIN32
        _exit(86);
#else
        raise(SIGKILL);
        _exit(86);
#endif
    }
    if (action == "exit") {
        _exit(86);
    }
    if (action == "throw") {
        throw DbError(ErrorCode::IO_ERROR, "Failpoint triggered: " + name);
    }
    throw DbError(ErrorCode::INVALID_ARGUMENT,
                  "RELDB_FAILPOINT action must be kill, exit, or throw");
}

bool FailPoint::IsConfigured(const std::string& name, const std::string& action) {
    const char* configured = std::getenv("RELDB_FAILPOINT");
    if (configured == nullptr) {
        return false;
    }
    const std::string requested(configured);
    const size_t separator = requested.find(':');
    return separator != std::string::npos && requested.substr(0, separator) == name &&
           requested.substr(separator + 1U) == action;
}

WriteAheadLog::WriteAheadLog(const std::string& database_path)
    : WriteAheadLog(database_path, ReadModeFromEnvironment(),
                    ReadUnsignedEnvironment("WAL_SYNC_INTERVAL_MS", 1000),
                    ReadUnsignedEnvironment("WAL_MAX_BYTES", 16U * 1024U * 1024U)) {}

WriteAheadLog::WriteAheadLog(const std::string& database_path, DurabilityMode mode,
                             uint64_t sync_interval_ms, uint64_t max_bytes)
    : database_path_(database_path),
      filename_(database_path + "-wal"),
      mode_(mode),
      sync_interval_ms_(sync_interval_ms),
      max_bytes_(max_bytes),
      last_sync_(std::chrono::steady_clock::now()) {
    if (sync_interval_ms_ == 0 || max_bytes_ == 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "WAL sync interval and maximum size must be positive");
    }
    if (mode_ == DurabilityMode::OFF) {
        const char* unsafe = std::getenv("ALLOW_UNSAFE");
        if (unsafe == nullptr || std::string(unsafe) != "1") {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "DURABILITY=off requires ALLOW_UNSAFE=1");
        }
    }
#ifdef _WIN32
    file_descriptor_ =
        _open(filename_.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    file_descriptor_ = open(filename_.c_str(),
                            O_RDWR | O_CREAT
#ifdef O_CLOEXEC
                                | O_CLOEXEC
#endif
                            ,
                            0644);
#endif
    if (file_descriptor_ < 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("open WAL"));
    }
    file_size_ = GetFileSize(file_descriptor_);
    if (file_size_ == 0) {
        const auto header = MakeHeader(0);
        WriteFullyAt(file_descriptor_, 0, header.data(), header.size());
        file_size_ = header.size();
        Sync();
        SyncParentDirectory(filename_);
    }
    if (mode_ == DurabilityMode::NORMAL) {
        sync_thread_ = std::thread(&WriteAheadLog::RunSyncTimer, this);
    }
}

WriteAheadLog::~WriteAheadLog() {
    {
        std::lock_guard<std::mutex> lock(latch_);
        stop_sync_thread_ = true;
    }
    sync_condition_.notify_all();
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
    if (file_descriptor_ >= 0) {
#ifdef _WIN32
        _close(file_descriptor_);
#else
        close(file_descriptor_);
#endif
    }
}

void WriteAheadLog::EnsureRecovered() const {
    if (!recovered_) {
        throw DbError(ErrorCode::INVALID_ARGUMENT,
                      "WAL recovery must complete before beginning write transactions");
    }
}

void WriteAheadLog::ThrowSyncError() const {
    if (!sync_error_.empty()) {
        throw DbError(ErrorCode::IO_ERROR, "WAL background sync failed: " + sync_error_);
    }
}

void WriteAheadLog::RunSyncTimer() {
    std::unique_lock<std::mutex> lock(latch_);
    while (!stop_sync_thread_) {
        if (sync_condition_.wait_for(lock, std::chrono::milliseconds(sync_interval_ms_),
                                     [this]() { return stop_sync_thread_; })) {
            break;
        }
        if (!recovered_ || !sync_error_.empty() || durable_lsn_ >= next_lsn_) {
            continue;
        }
        try {
            Sync();
            durable_lsn_ = next_lsn_;
            FailPoint::Hit("wal_fsync_after");
        } catch (const std::exception& error) {
            sync_error_ = error.what();
        }
    }
}

uint64_t WriteAheadLog::SizeBytes() const {
    std::lock_guard<std::mutex> lock(latch_);
    return file_size_;
}

uint64_t WriteAheadLog::GetFirstUnsyncedLSN() const {
    std::lock_guard<std::mutex> lock(latch_);
    return durable_lsn_;
}

bool WriteAheadLog::ShouldCheckpoint() const {
    std::lock_guard<std::mutex> lock(latch_);
    return file_size_ > max_bytes_;
}

void WriteAheadLog::EnsureDurable(uint64_t page_lsn) {
    if (mode_ == DurabilityMode::OFF) {
        return;
    }
    std::lock_guard<std::mutex> lock(latch_);
    EnsureRecovered();
    ThrowSyncError();
    if (page_lsn >= durable_lsn_) {
        try {
            Sync();
        } catch (const std::exception& error) {
            sync_error_ = error.what();
            throw;
        }
        durable_lsn_ = next_lsn_;
    }
}

std::vector<uint64_t> WriteAheadLog::Commit(uint64_t txn_id, const std::vector<CommitPage>& pages) {
    std::lock_guard<std::mutex> lock(latch_);
    EnsureRecovered();
    ThrowSyncError();

    std::vector<uint8_t> batch;
    std::vector<uint64_t> page_lsns;
    batch.reserve(pages.size() * (PAGE_SIZE + 32U) + 32U);
    page_lsns.reserve(pages.size());
    uint64_t lsn = next_lsn_;

    for (const CommitPage& page : pages) {
        FailPoint::Hit("page_image_before");
        const auto image = MakePageImage(page, lsn);
        std::vector<uint8_t> payload;
        payload.reserve(sizeof(uint32_t) + image.size());
        AppendU32(payload, static_cast<uint32_t>(page.page_id));
        payload.insert(payload.end(), image.begin(), image.end());

        const uint32_t record_length = static_cast<uint32_t>(kRecordFixedSize + payload.size());
        const size_t record_start = batch.size();
        AppendU32(batch, record_length);
        batch.push_back(static_cast<uint8_t>(LogRecordType::PAGE_IMAGE));
        AppendU64(batch, txn_id);
        AppendU64(batch, lsn);
        batch.insert(batch.end(), payload.begin(), payload.end());
        AppendU32(batch, Crc32c(batch.data() + record_start, batch.size() - record_start));
        page_lsns.push_back(lsn);
        ++lsn;
        FailPoint::Hit("page_image_after");
    }

    const uint32_t commit_length = static_cast<uint32_t>(kRecordFixedSize);
    const size_t commit_start = batch.size();
    AppendU32(batch, commit_length);
    batch.push_back(static_cast<uint8_t>(LogRecordType::COMMIT));
    AppendU64(batch, txn_id);
    AppendU64(batch, lsn);
    AppendU32(batch, Crc32c(batch.data() + commit_start, batch.size() - commit_start));
    ++lsn;

    WriteFullyAt(file_descriptor_, file_size_, batch.data(), batch.size());
    file_size_ += batch.size();
    log_count_ += pages.size() + 1U;
    next_lsn_ = lsn;
    next_txn_id_ = std::max(next_txn_id_, txn_id + 1U);
    FailPoint::Hit("commit_after");

    const auto now = std::chrono::steady_clock::now();
    const bool sync_now =
        mode_ == DurabilityMode::FULL ||
        (mode_ == DurabilityMode::NORMAL &&
         std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sync_).count() >=
             static_cast<int64_t>(sync_interval_ms_));
    if (sync_now) {
        try {
            Sync();
        } catch (const std::exception& error) {
            sync_error_ = error.what();
            throw;
        }
        durable_lsn_ = next_lsn_;
        FailPoint::Hit("wal_fsync_after");
    }
    return page_lsns;
}

void WriteAheadLog::Sync() {
    SyncDescriptor(file_descriptor_);
    last_sync_ = std::chrono::steady_clock::now();
}

WalRecoveryStats WriteAheadLog::Recover(IDiskManager& disk_manager) {
    std::lock_guard<std::mutex> lock(latch_);
    const auto started = std::chrono::steady_clock::now();
    ThrowSyncError();
    WalRecoveryStats stats{};

    if (file_size_ < kWalHeaderSize) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "WAL header is truncated");
    }
    std::array<uint8_t, kWalHeaderSize> header{};
    if (!ReadFullyAt(file_descriptor_, 0, header.data(), header.size())) {
        throw DbError(ErrorCode::IO_ERROR, "Unable to read WAL header");
    }
    if (!std::equal(kWalMagic.begin(), kWalMagic.end(), header.begin()) ||
        GetU32(header.data() + 8) != kWalVersion ||
        GetU32(header.data() + 20) != Crc32c(header.data(), 20)) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "WAL header magic, version, or CRC is invalid");
    }
    const uint64_t base_lsn = GetU64(header.data() + 12);
    uint64_t offset = kWalHeaderSize;
    uint64_t valid_end = offset;
    std::vector<ScannedRecord> records;
    uint64_t max_lsn = base_lsn;
    uint64_t expected_lsn = base_lsn;
    uint64_t max_txn_id = 0;

    while (offset + sizeof(uint32_t) <= file_size_) {
        std::array<uint8_t, 4> size_bytes{};
        if (!ReadFullyAt(file_descriptor_, offset, size_bytes.data(), size_bytes.size())) {
            break;
        }
        const uint32_t length = GetU32(size_bytes.data());
        if (length < kRecordFixedSize || length > PAGE_SIZE + kRecordFixedSize + sizeof(uint32_t) ||
            offset + length > file_size_) {
            break;
        }
        std::vector<uint8_t> record(length);
        if (!ReadFullyAt(file_descriptor_, offset, record.data(), record.size())) {
            break;
        }
        const uint32_t stored_crc = GetU32(record.data() + length - sizeof(uint32_t));
        if (stored_crc != Crc32c(record.data(), length - sizeof(uint32_t))) {
            break;
        }
        const uint8_t type_value = record[4];
        if (type_value < static_cast<uint8_t>(LogRecordType::PAGE_IMAGE) ||
            type_value > static_cast<uint8_t>(LogRecordType::CHECKPOINT)) {
            break;
        }
        const LogRecordType type = static_cast<LogRecordType>(type_value);
        const uint64_t txn_id = GetU64(record.data() + 5);
        const uint64_t lsn = GetU64(record.data() + 13);
        if (lsn != expected_lsn) {
            break;
        }
        const size_t payload_size = length - kRecordFixedSize;
        std::vector<uint8_t> payload(
            record.begin() + 21, record.begin() + static_cast<std::ptrdiff_t>(21U + payload_size));
        if ((type == LogRecordType::PAGE_IMAGE && payload_size != sizeof(uint32_t) + PAGE_SIZE) ||
            (type != LogRecordType::PAGE_IMAGE && payload_size != 0)) {
            break;
        }
        offset += length;
        valid_end = offset;
        max_lsn = lsn;
        if (expected_lsn == std::numeric_limits<uint64_t>::max()) {
            break;
        }
        ++expected_lsn;
        max_txn_id = std::max(max_txn_id, txn_id);
        records.push_back(ScannedRecord{type, txn_id, lsn, std::move(payload), offset});
    }

    std::unordered_map<uint64_t, uint64_t> committed_at_lsn;
    for (const ScannedRecord& record : records) {
        if (record.type == LogRecordType::COMMIT) {
            committed_at_lsn[record.txn_id] = record.lsn;
            ++stats.commits;
        }
    }
    for (const ScannedRecord& record : records) {
        if (record.type != LogRecordType::PAGE_IMAGE) {
            continue;
        }
        const auto commit = committed_at_lsn.find(record.txn_id);
        if (commit == committed_at_lsn.end() || record.lsn >= commit->second) {
            continue;
        }
        const page_id_t page_id = static_cast<page_id_t>(GetU32(record.payload.data()));
        disk_manager.WritePageImage(page_id, record.payload.data() + sizeof(uint32_t));
        ++stats.pages_replayed;
        if (stats.pages_replayed == 1) {
            FailPoint::Hit("recovery_mid");
        }
    }
    disk_manager.Sync();
    stats.records = records.size();

    const uint64_t truncated_from = file_size_;
    const uint64_t new_base_lsn = records.empty() ? base_lsn : max_lsn + 1U;
    FailPoint::Hit("wal_before_truncate");
    RewriteHeader(new_base_lsn);
    stats.truncated_bytes = truncated_from > kWalHeaderSize ? truncated_from - kWalHeaderSize : 0;
    next_lsn_ = new_base_lsn;
    durable_lsn_ = new_base_lsn;
    next_txn_id_ = std::max(next_txn_id_, max_txn_id + 1U);
    log_count_ = 0;
    recovered_ = true;
    stats.elapsed_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - started)
                                                 .count());
    std::clog << "[WAL] recovery records=" << stats.records << " commits=" << stats.commits
              << " pages_replayed=" << stats.pages_replayed
              << " truncated_bytes=" << stats.truncated_bytes << " ms=" << stats.elapsed_ms << '\n';
    (void)valid_end;
    return stats;
}

void WriteAheadLog::Checkpoint(BufferPoolManager& buffer_pool) {
    EnsureRecovered();
    buffer_pool.BeginCheckpoint(*this);
    try {
        buffer_pool.FlushAll();
        FailPoint::Hit("checkpoint_db_fsync_after");
        std::lock_guard<std::mutex> lock(latch_);
        ThrowSyncError();
        const uint64_t checkpoint_lsn = next_lsn_++;
        std::vector<uint8_t> record;
        AppendU32(record, static_cast<uint32_t>(kRecordFixedSize));
        record.push_back(static_cast<uint8_t>(LogRecordType::CHECKPOINT));
        AppendU64(record, 0);
        AppendU64(record, checkpoint_lsn);
        AppendU32(record, Crc32c(record.data(), record.size()));
        WriteFullyAt(file_descriptor_, file_size_, record.data(), record.size());
        file_size_ += record.size();
        Sync();
        FailPoint::Hit("checkpoint_before_truncate");
        FailPoint::Hit("wal_before_truncate");
        RewriteHeader(next_lsn_);
        durable_lsn_ = next_lsn_;
        log_count_ = 0;
    } catch (...) {
        buffer_pool.EndCheckpoint();
        throw;
    }
    buffer_pool.EndCheckpoint();
}

void WriteAheadLog::RewriteHeader(uint64_t base_lsn) {
    const auto header = MakeHeader(base_lsn);
#ifdef _WIN32
    if (_chsize_s(file_descriptor_, 0) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("truncate WAL"));
    }
    WriteFullyAt(file_descriptor_, 0, header.data(), header.size());
    Sync();
    file_size_ = header.size();
#else
    const std::string temporary = filename_ + ".tmp";
    const int replacement = open(temporary.c_str(),
                                 O_RDWR | O_CREAT | O_TRUNC
#ifdef O_CLOEXEC
                                     | O_CLOEXEC
#endif
                                 ,
                                 0644);
    if (replacement < 0) {
        throw DbError(ErrorCode::IO_ERROR, IoMessage("open temporary WAL"));
    }
    try {
        WriteFullyAt(replacement, 0, header.data(), header.size());
        SyncDescriptor(replacement);
        if (rename(temporary.c_str(), filename_.c_str()) != 0) {
            throw DbError(ErrorCode::IO_ERROR, IoMessage("atomically replace WAL"));
        }
        SyncParentDirectory(filename_);
        close(file_descriptor_);
        file_descriptor_ = open(filename_.c_str(), O_RDWR
#ifdef O_CLOEXEC
                                                       | O_CLOEXEC
#endif
        );
        if (file_descriptor_ < 0) {
            throw DbError(ErrorCode::IO_ERROR, IoMessage("reopen WAL"));
        }
        file_size_ = header.size();
        last_sync_ = std::chrono::steady_clock::now();
    } catch (...) {
        close(replacement);
        throw;
    }
    close(replacement);
#endif
}

}  // namespace relational_db
