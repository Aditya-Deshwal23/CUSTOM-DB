#ifndef WAL_H
#define WAL_H

#include "config.h"
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

namespace relational_db {

enum class LogRecordType : uint8_t {
    INVALID = 0, INSERT  = 1, DEL = 2, UPDATE = 3, BEGIN = 4, COMMIT = 5, ABORT = 6
};

struct LogRecord {
    uint64_t lsn;
    uint64_t prev_lsn;
    LogRecordType type;
    page_id_t page_id;
    uint32_t length;
    std::vector<uint8_t> data;
    uint32_t crc32;

    uint32_t SerializedSize() const {
        return 25 + static_cast<uint32_t>(data.size()) + 4;
    }
};

class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& filename);
    ~WriteAheadLog();

    uint64_t AppendLog(LogRecord& record);
    void Flush();
    std::vector<LogRecord> Recovery();

    uint64_t GetNextLSN() const { return next_lsn_; }
    uint64_t GetLogCount() const { return log_count_; }
    static uint32_t ComputeCRC32(const uint8_t* data, size_t length);

private:
    std::string filename_;
    int file_descriptor_{-1};

    // Explicit standard mutex
    std::mutex latch_;
    
    uint64_t next_lsn_{0};
    uint64_t log_count_{0};
};

} // namespace relational_db

#endif // WAL_H
