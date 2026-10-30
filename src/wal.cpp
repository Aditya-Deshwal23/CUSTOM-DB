#include "wal.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace relational_db {
namespace {

bool ReadFully(int descriptor, uint8_t* buffer, size_t length) {
    size_t transferred = 0;
    while (transferred < length) {
#ifdef _WIN32
        const unsigned int remaining = static_cast<unsigned int>(length - transferred);
        const int amount = _read(descriptor, buffer + transferred, remaining);
#else
        const ssize_t amount = read(descriptor, buffer + transferred, length - transferred);
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

void WriteFully(int descriptor, const uint8_t* buffer, size_t length) {
    size_t transferred = 0;
    while (transferred < length) {
#ifdef _WIN32
        const unsigned int remaining = static_cast<unsigned int>(length - transferred);
        const int amount = _write(descriptor, buffer + transferred, remaining);
#else
        const ssize_t amount = write(descriptor, buffer + transferred, length - transferred);
#endif
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            throw std::runtime_error(std::string("WAL write failed: ") + std::strerror(errno));
        }
        transferred += static_cast<size_t>(amount);
    }
}

}  // namespace

uint32_t WriteAheadLog::ComputeCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFF;
}

WriteAheadLog::WriteAheadLog(const std::string& filename) : filename_(filename) {
#ifdef _WIN32
    file_descriptor_ = _open(filename_.c_str(), _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY,
                             _S_IREAD | _S_IWRITE);
#else
    file_descriptor_ = open(filename_.c_str(),
                            O_WRONLY | O_APPEND | O_CREAT
#ifdef O_CLOEXEC
                                | O_CLOEXEC
#endif
                            ,
                            0644);
#endif
    if (file_descriptor_ < 0) {
        throw std::runtime_error("Failed to open WAL file: " + filename_ + ": " +
                                 std::strerror(errno));
    }
    std::cout << "[WAL] Opened write-ahead log: " << filename_ << std::endl;
}

WriteAheadLog::~WriteAheadLog() {
    if (file_descriptor_ >= 0) {
        try {
            Flush();
        } catch (const std::exception& error) {
            std::cerr << "[WAL] Final sync failed: " << error.what() << std::endl;
        }
#ifdef _WIN32
        _close(file_descriptor_);
#else
        close(file_descriptor_);
#endif
    }
}

uint64_t WriteAheadLog::AppendLog(LogRecord& record) {
    std::lock_guard<std::mutex> lock(latch_);

    record.lsn = next_lsn_++;
    record.length = static_cast<uint32_t>(record.data.size());

    std::vector<uint8_t> buffer;
    buffer.reserve(record.SerializedSize());

    auto appendBytes = [&buffer](const void* src, size_t len) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(src);
        buffer.insert(buffer.end(), bytes, bytes + len);
    };

    appendBytes(&record.lsn, sizeof(record.lsn));
    appendBytes(&record.prev_lsn, sizeof(record.prev_lsn));
    appendBytes(&record.type, sizeof(record.type));
    appendBytes(&record.page_id, sizeof(record.page_id));
    appendBytes(&record.length, sizeof(record.length));
    if (!record.data.empty()) {
        buffer.insert(buffer.end(), record.data.begin(), record.data.end());
    }

    record.crc32 = ComputeCRC32(buffer.data(), buffer.size());
    appendBytes(&record.crc32, sizeof(record.crc32));

    WriteFully(file_descriptor_, buffer.data(), buffer.size());
    log_count_++;
    return record.lsn;
}

void WriteAheadLog::Flush() {
    std::lock_guard<std::mutex> lock(latch_);
#ifdef _WIN32
    if (_commit(file_descriptor_) != 0) {
        throw std::runtime_error(std::string("WAL commit failed: ") + std::strerror(errno));
    }
#else
    if (fsync(file_descriptor_) != 0) {
        throw std::runtime_error(std::string("WAL fsync failed: ") + std::strerror(errno));
    }
#endif
}

std::vector<LogRecord> WriteAheadLog::Recovery() {
    std::vector<LogRecord> records;
#ifdef _WIN32
    const int descriptor = _open(filename_.c_str(), _O_RDONLY | _O_BINARY);
#else
    const int descriptor = open(filename_.c_str(), O_RDONLY
#ifdef O_CLOEXEC
                                                       | O_CLOEXEC
#endif
    );
#endif
    if (descriptor < 0) {
        if (errno == ENOENT) {
            std::cout << "[WAL] No WAL file found - clean start." << std::endl;
            return records;
        }
        throw std::runtime_error(std::string("Failed to open WAL for recovery: ") +
                                 std::strerror(errno));
    }

    std::cout << "[WAL] Starting crash recovery from: " << filename_ << std::endl;
    while (true) {
        LogRecord record{};
        if (!ReadFully(descriptor, reinterpret_cast<uint8_t*>(&record.lsn), sizeof(record.lsn)) ||
            !ReadFully(descriptor, reinterpret_cast<uint8_t*>(&record.prev_lsn),
                       sizeof(record.prev_lsn)) ||
            !ReadFully(descriptor, reinterpret_cast<uint8_t*>(&record.type), sizeof(record.type)) ||
            !ReadFully(descriptor, reinterpret_cast<uint8_t*>(&record.page_id),
                       sizeof(record.page_id)) ||
            !ReadFully(descriptor, reinterpret_cast<uint8_t*>(&record.length),
                       sizeof(record.length))) {
            break;
        }

        if (record.length > PAGE_SIZE * 2) {
            std::cout << "[WAL] Corrupt record length " << record.length << ". Stopping."
                      << std::endl;
            break;
        }

        record.data.resize(record.length);
        if (record.length > 0 && !ReadFully(descriptor, record.data.data(), record.length)) {
            break;
        }

        uint32_t stored_crc = 0;
        if (!ReadFully(descriptor, reinterpret_cast<uint8_t*>(&stored_crc), sizeof(stored_crc))) {
            break;
        }

        std::vector<uint8_t> verify_buf;
        auto appendBytes = [&verify_buf](const void* src, size_t len) {
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(src);
            verify_buf.insert(verify_buf.end(), bytes, bytes + len);
        };

        appendBytes(&record.lsn, sizeof(record.lsn));
        appendBytes(&record.prev_lsn, sizeof(record.prev_lsn));
        appendBytes(&record.type, sizeof(record.type));
        appendBytes(&record.page_id, sizeof(record.page_id));
        appendBytes(&record.length, sizeof(record.length));
        if (!record.data.empty()) {
            verify_buf.insert(verify_buf.end(), record.data.begin(), record.data.end());
        }

        const uint32_t computed_crc = ComputeCRC32(verify_buf.data(), verify_buf.size());
        if (computed_crc != stored_crc) {
            std::cout << "[WAL] CRC mismatch at LSN " << record.lsn << ". Stopping." << std::endl;
            break;
        }

        record.crc32 = stored_crc;
        records.push_back(std::move(record));
        if (record.lsn >= next_lsn_) {
            next_lsn_ = record.lsn + 1;
        }
    }
#ifdef _WIN32
    _close(descriptor);
#else
    close(descriptor);
#endif
    std::cout << "[WAL] Recovery complete. " << records.size() << " records found." << std::endl;
    return records;
}

}  // namespace relational_db
