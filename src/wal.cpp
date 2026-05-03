#include "wal.h"
#include <iostream>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

namespace relational_db {

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

WriteAheadLog::WriteAheadLog(const std::string& filename)
    : filename_(filename) {
    wal_file_.open(filename_, std::ios::binary | std::ios::app);
    if (!wal_file_.is_open()) {
        throw std::runtime_error("Failed to open WAL file: " + filename_);
    }
    std::cout << "[WAL] Opened write-ahead log: " << filename_ << std::endl;
}

WriteAheadLog::~WriteAheadLog() {
    if (wal_file_.is_open()) {
        Flush();
        wal_file_.close();
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

    wal_file_.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());

    log_count_++;
    return record.lsn;
}

void WriteAheadLog::Flush() {
    std::lock_guard<std::mutex> lock(latch_);
    wal_file_.flush();
}

std::vector<LogRecord> WriteAheadLog::Recovery() {
    std::vector<LogRecord> records;
    std::ifstream infile(filename_, std::ios::binary);
    if (!infile.is_open()) {
        std::cout << "[WAL] No WAL file found — clean start." << std::endl;
        return records;
    }

    std::cout << "[WAL] Starting crash recovery from: " << filename_ << std::endl;

    while (infile.good() && infile.peek() != EOF) {
        LogRecord record;
        infile.read(reinterpret_cast<char*>(&record.lsn), sizeof(record.lsn));
        infile.read(reinterpret_cast<char*>(&record.prev_lsn), sizeof(record.prev_lsn));
        infile.read(reinterpret_cast<char*>(&record.type), sizeof(record.type));
        infile.read(reinterpret_cast<char*>(&record.page_id), sizeof(record.page_id));
        infile.read(reinterpret_cast<char*>(&record.length), sizeof(record.length));

        if (!infile.good()) break;

        if (record.length > PAGE_SIZE * 2) {
            std::cout << "[WAL] Corrupt record length " << record.length << ". Stopping." << std::endl;
            break;
        }

        record.data.resize(record.length);
        if (record.length > 0) {
            infile.read(reinterpret_cast<char*>(record.data.data()), record.length);
            if (!infile.good()) break;
        }

        uint32_t stored_crc;
        infile.read(reinterpret_cast<char*>(&stored_crc), sizeof(stored_crc));
        if (!infile.good()) break;

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

        uint32_t computed_crc = ComputeCRC32(verify_buf.data(), verify_buf.size());
        if (computed_crc != stored_crc) {
            std::cout << "[WAL] CRC mismatch at LSN " << record.lsn << ". Stopping." << std::endl;
            break;
        }

        record.crc32 = stored_crc;
        records.push_back(std::move(record));
        if (record.lsn >= next_lsn_) next_lsn_ = record.lsn + 1;
    }
    std::cout << "[WAL] Recovery complete. " << records.size() << " records found." << std::endl;
    return records;
}

} // namespace relational_db
