#include "disk_manager.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

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
#if defined(__APPLE__)
#include <sys/fcntl.h>
#endif
#endif

namespace relational_db {
namespace {

constexpr uint32_t kFormatVersion = 2;
constexpr uint64_t kGrowthPages = 64;
constexpr size_t kDatabaseHeaderOffset = PAGE_HEADER_SIZE;
constexpr size_t kMagicOffset = kDatabaseHeaderOffset;
constexpr size_t kVersionOffset = kMagicOffset + 8;
constexpr size_t kPageSizeOffset = kVersionOffset + 4;
constexpr size_t kPageCountOffset = kPageSizeOffset + 4;
constexpr size_t kFreeListHeadOffset = kPageCountOffset + 8;
constexpr size_t kCatalogRootOffset = kFreeListHeadOffset + 4;
constexpr size_t kHeaderCrcOffset = kCatalogRootOffset + 4;
constexpr size_t kDatabaseHeaderSerializedSize = kHeaderCrcOffset + 4 - kDatabaseHeaderOffset;
constexpr uint8_t kMagic[8] = {'R', 'E', 'L', 'D', 'B', 0, 0, 2};
#ifdef _WIN32
std::mutex g_windows_io_mutex;
#endif

struct DatabaseHeaderLayout {
    uint8_t magic[8];
    uint32_t format_version;
    uint32_t page_size;
    uint64_t page_count;
    int32_t free_list_head;
    int32_t catalog_root;
    uint32_t crc32c;
};

static_assert(offsetof(DatabaseHeaderLayout, magic) == 0, "DB magic offset changed");
static_assert(offsetof(DatabaseHeaderLayout, format_version) == 8, "DB version offset changed");
static_assert(offsetof(DatabaseHeaderLayout, page_size) == 12, "DB page-size offset changed");
static_assert(offsetof(DatabaseHeaderLayout, page_count) == 16, "DB page-count offset changed");
static_assert(offsetof(DatabaseHeaderLayout, free_list_head) == 24, "DB free-list offset changed");
static_assert(offsetof(DatabaseHeaderLayout, catalog_root) == 28, "DB catalog offset changed");
static_assert(offsetof(DatabaseHeaderLayout, crc32c) == 32, "DB CRC offset changed");
static_assert(sizeof(DatabaseHeaderLayout) == 40, "DB header layout changed");
static_assert(kDatabaseHeaderSerializedSize == 36, "Serialized DB header layout changed");

uint16_t GetU16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

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

std::string IoErrorMessage(const std::string& operation) {
    return operation + ": " + std::strerror(errno);
}

uint64_t PageOffset(page_id_t page_id) {
    return static_cast<uint64_t>(page_id) * PAGE_SIZE;
}

#ifndef _WIN32
void SyncParentDirectory(const std::string& path) {
    const size_t separator = path.find_last_of('/');
    const std::string directory = separator == std::string::npos ? "."
                                  : separator == 0               ? "/"
                                                                 : path.substr(0, separator);
    const int directory_fd = open(directory.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                                         | O_DIRECTORY
#endif
    );
    if (directory_fd < 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("open parent directory"));
    }
    const int result = fsync(directory_fd);
    const int sync_error = errno;
    close(directory_fd);
    if (result != 0) {
        errno = sync_error;
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("fsync parent directory"));
    }
}
#endif

}  // namespace

FileDiskManager::FileDiskManager(const std::string& db_file) : file_name_(db_file) {
    Open();
}

FileDiskManager::~FileDiskManager() {
    if (file_descriptor_ >= 0) {
#ifdef _WIN32
        _close(file_descriptor_);
#else
        close(file_descriptor_);
#endif
    }
}

void FileDiskManager::Open() {
    std::lock_guard<std::mutex> open_lock(open_mutex_);
    if (opened_.load(std::memory_order_acquire)) {
        return;
    }

    bool created = false;
#ifdef _WIN32
    file_descriptor_ =
        _open(file_name_.c_str(), _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (file_descriptor_ >= 0) {
        created = true;
    } else if (errno == EEXIST) {
        file_descriptor_ = _open(file_name_.c_str(), _O_RDWR | _O_BINARY);
    }
#else
    file_descriptor_ = open(file_name_.c_str(),
                            O_RDWR | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                                | O_CLOEXEC
#endif
                            ,
                            0644);
    if (file_descriptor_ >= 0) {
        created = true;
    } else if (errno == EEXIST) {
        file_descriptor_ = open(file_name_.c_str(), O_RDWR
#ifdef O_CLOEXEC
                                                        | O_CLOEXEC
#endif
        );
    }
#endif
    if (file_descriptor_ < 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("open database file"));
    }

    try {
        if (created) {
            InitializeNewFile();
#ifndef _WIN32
            SyncParentDirectory(file_name_);
#endif
            Sync();
        } else {
            ValidateExistingFile();
        }
        opened_.store(true, std::memory_order_release);
    } catch (...) {
#ifdef _WIN32
        _close(file_descriptor_);
#else
        close(file_descriptor_);
#endif
        file_descriptor_ = -1;
        throw;
    }
}

void FileDiskManager::InitializeNewFile() {
    page_count_.store(1, std::memory_order_release);
    GrowPhysicalFile(1);
    WriteDatabaseHeader(1);
}

void FileDiskManager::ValidateExistingFile() {
#ifdef _WIN32
    const __int64 existing_file_size = _filelengthi64(file_descriptor_);
#else
    struct stat status{};
    if (fstat(file_descriptor_, &status) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("stat database file"));
    }
    const uint64_t existing_file_size = static_cast<uint64_t>(status.st_size);
#endif
    if (existing_file_size < PAGE_SIZE) {
        throw DbError(ErrorCode::INVALID_DB_FILE,
                      "Database file is too short to contain a page-zero header");
    }
    std::array<uint8_t, PAGE_SIZE> image{};
    ReadExact(0, reinterpret_cast<char*>(image.data()), image.size(), 0);

    if (!std::equal(std::begin(kMagic), std::end(kMagic),
                    image.begin() + static_cast<std::ptrdiff_t>(kMagicOffset))) {
        throw DbError(ErrorCode::INVALID_DB_FILE, "Database file has invalid magic");
    }
    const uint32_t version = GetU32(image.data() + kVersionOffset);
    if (version != kFormatVersion) {
        throw DbError(ErrorCode::UNSUPPORTED_VERSION,
                      "Unsupported database file format version " + std::to_string(version));
    }
    if (GetU32(image.data() + kPageSizeOffset) != PAGE_SIZE) {
        throw DbError(ErrorCode::HEADER_CORRUPT, "Database page size is invalid");
    }

    const uint32_t stored_header_crc = GetU32(image.data() + kHeaderCrcOffset);
    const uint32_t computed_header_crc =
        Crc32c(image.data() + kMagicOffset, kHeaderCrcOffset - kMagicOffset);
    if (stored_header_crc != computed_header_crc) {
        throw DbError(ErrorCode::HEADER_CORRUPT, "Database header CRC32C mismatch");
    }
    const uint32_t stored_page_crc = GetU32(image.data());
    if (GetU16(image.data() + 4) != static_cast<uint16_t>(PageType::HEADER) ||
        stored_page_crc != Crc32c(image.data() + 4, PAGE_SIZE - 4)) {
        throw DbError(ErrorCode::HEADER_CORRUPT, "Database page-zero CRC32C mismatch");
    }

    const uint64_t count = GetU64(image.data() + kPageCountOffset);
    if (count == 0 || count > static_cast<uint64_t>(std::numeric_limits<page_id_t>::max()) + 1U) {
        throw DbError(ErrorCode::HEADER_CORRUPT, "Database page count is invalid");
    }
#ifdef _WIN32
    const uint64_t file_size =
        existing_file_size < 0 ? 0 : static_cast<uint64_t>(existing_file_size);
#else
    const uint64_t file_size = existing_file_size;
#endif
    if (file_size < count * PAGE_SIZE) {
        throw DbError(ErrorCode::HEADER_CORRUPT,
                      "Database file is shorter than its logical page count");
    }
    page_count_.store(count, std::memory_order_release);
}

void FileDiskManager::WriteDatabaseHeader(uint64_t page_count) {
    std::array<uint8_t, PAGE_SIZE> image{};
    PutU16(image.data() + 4, static_cast<uint16_t>(PageType::HEADER));
    std::copy(std::begin(kMagic), std::end(kMagic),
              image.begin() + static_cast<std::ptrdiff_t>(kMagicOffset));
    PutU32(image.data() + kVersionOffset, kFormatVersion);
    PutU32(image.data() + kPageSizeOffset, PAGE_SIZE);
    PutU64(image.data() + kPageCountOffset, page_count);
    PutU32(image.data() + kFreeListHeadOffset, static_cast<uint32_t>(INVALID_PAGE_ID));
    PutU32(image.data() + kCatalogRootOffset, static_cast<uint32_t>(INVALID_PAGE_ID));
    PutU32(image.data() + kHeaderCrcOffset,
           Crc32c(image.data() + kMagicOffset, kHeaderCrcOffset - kMagicOffset));
    PutU32(image.data(), Crc32c(image.data() + 4, PAGE_SIZE - 4));
    WriteExact(0, reinterpret_cast<const char*>(image.data()), image.size());
    page_count_.store(page_count, std::memory_order_release);
}

void FileDiskManager::GrowPhysicalFile(uint64_t logical_page_count) {
    const uint64_t rounded_pages =
        ((logical_page_count + kGrowthPages - 1) / kGrowthPages) * kGrowthPages;
    if (rounded_pages > std::numeric_limits<uint64_t>::max() / PAGE_SIZE) {
        throw DbError(ErrorCode::IO_ERROR, "Requested database file size overflows");
    }
    const uint64_t target_size = rounded_pages * PAGE_SIZE;
#ifdef _WIN32
    const int64_t current_size = _filelengthi64(file_descriptor_);
    if (current_size < 0 || static_cast<uint64_t>(current_size) < target_size) {
        const errno_t result = _chsize_s(file_descriptor_, target_size);
        if (result != 0) {
            errno = static_cast<int>(result);
            throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("grow database file"));
        }
    }
#else
    struct stat status{};
    if (fstat(file_descriptor_, &status) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("stat database file"));
    }
    if (static_cast<uint64_t>(status.st_size) < target_size) {
        if (ftruncate(file_descriptor_, static_cast<off_t>(target_size)) != 0) {
            throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("grow database file"));
        }
    }
#endif
}

void FileDiskManager::ReadExact(uint64_t offset, char* buffer, size_t length, page_id_t page_id) {
    const auto start = std::chrono::steady_clock::now();
    size_t transferred = 0;
#ifdef _WIN32
    std::lock_guard<std::mutex> io_lock(g_windows_io_mutex);
    if (_lseeki64(file_descriptor_, static_cast<__int64>(offset), SEEK_SET) < 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("seek database file"), page_id);
    }
    while (transferred < length) {
        const unsigned int remaining = static_cast<unsigned int>(std::min(
            length - transferred, static_cast<size_t>(std::numeric_limits<unsigned int>::max())));
        const int amount = _read(file_descriptor_, buffer + transferred, remaining);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            throw DbError(ErrorCode::IO_ERROR, "Short read from database file", page_id);
        }
        transferred += static_cast<size_t>(amount);
    }
#else
    while (transferred < length) {
        const ssize_t amount = pread(file_descriptor_, buffer + transferred, length - transferred,
                                     static_cast<off_t>(offset + transferred));
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("read database file"), page_id);
        }
        transferred += static_cast<size_t>(amount);
    }
#endif
    reads_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(length, std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    RecordLatency(read_latency_us_, static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)));
}

void FileDiskManager::WriteExact(uint64_t offset, const char* buffer, size_t length) {
    const auto start = std::chrono::steady_clock::now();
    size_t transferred = 0;
#ifdef _WIN32
    std::lock_guard<std::mutex> io_lock(g_windows_io_mutex);
    if (_lseeki64(file_descriptor_, static_cast<__int64>(offset), SEEK_SET) < 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("seek database file"));
    }
    while (transferred < length) {
        const unsigned int remaining = static_cast<unsigned int>(std::min(
            length - transferred, static_cast<size_t>(std::numeric_limits<unsigned int>::max())));
        const int amount = _write(file_descriptor_, buffer + transferred, remaining);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("write database file"));
        }
        transferred += static_cast<size_t>(amount);
    }
#else
    while (transferred < length) {
        const ssize_t amount = pwrite(file_descriptor_, buffer + transferred, length - transferred,
                                      static_cast<off_t>(offset + transferred));
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("write database file"));
        }
        transferred += static_cast<size_t>(amount);
    }
#endif
    writes_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(length, std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    RecordLatency(write_latency_us_, static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)));
}

void FileDiskManager::ReadPage(page_id_t page_id, char* page_data) {
    if (page_id < 0 || static_cast<uint64_t>(page_id) >= SizeInPages()) {
        throw DbError(ErrorCode::PAGE_OUT_OF_RANGE, "Page ID is outside the database file",
                      page_id);
    }
    if (page_data == nullptr) {
        throw DbError(ErrorCode::IO_ERROR, "ReadPage requires a non-null output buffer", page_id);
    }

    std::unique_lock<std::mutex> header_lock(header_mutex_, std::defer_lock);
    if (page_id == 0) {
        header_lock.lock();
    }
    std::array<uint8_t, PAGE_SIZE> image{};
    ReadExact(PageOffset(page_id), reinterpret_cast<char*>(image.data()), image.size(), page_id);
    if (std::all_of(image.begin(), image.end(), [](uint8_t value) { return value == 0; })) {
        std::memset(page_data, 0, PAGE_DATA_SIZE);
        return;
    }

    const uint32_t stored_crc = GetU32(image.data());
    const uint32_t computed_crc = Crc32c(image.data() + 4, PAGE_SIZE - 4);
    if (stored_crc != computed_crc) {
        throw DbError(ErrorCode::PAGE_CHECKSUM_MISMATCH,
                      "Page CRC32C mismatch for page " + std::to_string(page_id), page_id);
    }
    std::memcpy(page_data, image.data() + PAGE_HEADER_SIZE, PAGE_DATA_SIZE);
}

void FileDiskManager::WritePage(page_id_t page_id, const char* page_data, PageType type,
                                uint16_t flags, uint64_t lsn) {
    if (page_id == 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Page zero is reserved for the database header",
                      page_id);
    }
    if (page_id < 0 || static_cast<uint64_t>(page_id) >= SizeInPages()) {
        throw DbError(ErrorCode::PAGE_OUT_OF_RANGE, "Page ID is outside the database file",
                      page_id);
    }
    if (page_data == nullptr) {
        throw DbError(ErrorCode::IO_ERROR, "WritePage requires a non-null input buffer", page_id);
    }

    std::array<uint8_t, PAGE_SIZE> image{};
    PutU16(image.data() + 4, static_cast<uint16_t>(type));
    PutU16(image.data() + 6, flags);
    PutU64(image.data() + 8, lsn);
    std::memcpy(image.data() + PAGE_HEADER_SIZE, page_data, PAGE_DATA_SIZE);
    PutU32(image.data(), Crc32c(image.data() + 4, PAGE_SIZE - 4));
    WriteExact(PageOffset(page_id), reinterpret_cast<const char*>(image.data()), image.size());
}

void FileDiskManager::WriteHeaderPage(const char* page_data) {
    if (page_data == nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "WriteHeaderPage requires a non-null input");
    }

    std::lock_guard<std::mutex> lock(header_mutex_);
    std::array<uint8_t, PAGE_SIZE> image{};
    ReadExact(0, reinterpret_cast<char*>(image.data()), image.size(), 0);
    std::memcpy(image.data() + PAGE_HEADER_SIZE, page_data, PAGE_DATA_SIZE);
    PutU32(image.data() + kHeaderCrcOffset,
           Crc32c(image.data() + kMagicOffset, kHeaderCrcOffset - kMagicOffset));
    PutU32(image.data(), Crc32c(image.data() + 4, PAGE_SIZE - 4));
    WriteExact(0, reinterpret_cast<const char*>(image.data()), image.size());
    page_count_.store(GetU64(image.data() + kPageCountOffset), std::memory_order_release);
}

void FileDiskManager::Sync() {
    const auto start = std::chrono::steady_clock::now();
#ifdef _WIN32
    if (_commit(file_descriptor_) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("commit database file"));
    }
#elif defined(__APPLE__)
    if (fcntl(file_descriptor_, F_FULLFSYNC) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("full sync database file"));
    }
#else
    if (fdatasync(file_descriptor_) != 0) {
        throw DbError(ErrorCode::IO_ERROR, IoErrorMessage("fdatasync database file"));
    }
#endif
    syncs_.fetch_add(1, std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    RecordLatency(sync_latency_us_, static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)));
}

uint64_t FileDiskManager::SizeInPages() const {
    return page_count_.load(std::memory_order_acquire);
}

void FileDiskManager::Extend(uint64_t page_count) {
    if (page_count == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(header_mutex_);
    const uint64_t current = page_count_.load(std::memory_order_relaxed);
    const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<page_id_t>::max()) + 1U;
    if (page_count > maximum - current) {
        throw DbError(ErrorCode::IO_ERROR, "Requested database page count exceeds page IDs");
    }
    const uint64_t updated = current + page_count;
    GrowPhysicalFile(updated);
    std::array<uint8_t, PAGE_SIZE> image{};
    ReadExact(0, reinterpret_cast<char*>(image.data()), image.size(), 0);
    PutU64(image.data() + kPageCountOffset, updated);
    PutU32(image.data() + kHeaderCrcOffset,
           Crc32c(image.data() + kMagicOffset, kHeaderCrcOffset - kMagicOffset));
    PutU32(image.data(), Crc32c(image.data() + 4, PAGE_SIZE - 4));
    WriteExact(0, reinterpret_cast<const char*>(image.data()), image.size());
    page_count_.store(updated, std::memory_order_release);
}

DiskStats FileDiskManager::Stats() const {
    DiskStats snapshot{};
    snapshot.reads = reads_.load(std::memory_order_relaxed);
    snapshot.writes = writes_.load(std::memory_order_relaxed);
    snapshot.syncs = syncs_.load(std::memory_order_relaxed);
    snapshot.bytes = bytes_.load(std::memory_order_relaxed);
    for (size_t index = 0; index < DiskStats::kLatencyBucketCount; ++index) {
        snapshot.read_latency_us[index] = read_latency_us_[index].load(std::memory_order_relaxed);
        snapshot.write_latency_us[index] = write_latency_us_[index].load(std::memory_order_relaxed);
        snapshot.sync_latency_us[index] = sync_latency_us_[index].load(std::memory_order_relaxed);
    }
    return snapshot;
}

void FileDiskManager::RecordLatency(
    std::array<std::atomic<uint64_t>, DiskStats::kLatencyBucketCount>& buckets,
    uint64_t elapsed_us) {
    size_t bucket = 0;
    while (elapsed_us > 1 && bucket + 1 < buckets.size()) {
        elapsed_us >>= 1U;
        ++bucket;
    }
    buckets[bucket].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace relational_db
