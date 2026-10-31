#ifndef CONFIG_H
#define CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace relational_db {

// Define the global page size (e.g., 4KB)
constexpr static const uint32_t PAGE_SIZE = 4096;
constexpr static const uint32_t PAGE_HEADER_SIZE = 16;
constexpr static const uint32_t PAGE_DATA_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE;

// Type alias for page identifiers
using page_id_t = int32_t;
using frame_id_t = int32_t;

// Invalid page ID to indicate null/empty
constexpr static const page_id_t INVALID_PAGE_ID = -1;

constexpr uint64_t MAX_RESULT_ROWS = 100000;
constexpr uint64_t MAX_SORT_ROWS = 100000;
constexpr uint64_t STATEMENT_TIMEOUT_MS = 5000;
constexpr uint64_t STATEMENT_GATE_TIMEOUT_MS = 1000;
constexpr uint64_t MAX_CONCURRENT_STATEMENTS = 64;
constexpr uint64_t MAX_QUEUED_STATEMENTS = 128;

struct Config {
    uint16_t port{8080};
    std::string db_path{"./test.db"};
    std::string wal_path;
    uint64_t buffer_pool_frames{4096};
    std::string buffer_pool_policy{"lruk"};
    std::string durability{"full"};
    uint64_t wal_max_bytes{16U * 1024U * 1024U};
    uint64_t wal_sync_interval_ms{1000};
    bool read_only{false};
    std::string api_key;
    std::vector<std::string> cors_origins;
    uint64_t max_body_bytes{64U * 1024U};
    uint64_t max_sql_bytes{16U * 1024U};
    uint64_t max_result_rows{1000};
    uint64_t statement_timeout_ms{2000};
    double rate_limit_rps{20.0};
    double rate_limit_burst{40.0};
    uint64_t max_db_bytes{0};
    std::string log_level{"info"};
    bool demo_load{false};
    uint64_t slow_query_ms{0};
    uint64_t drain_timeout_ms{5000};
    bool allow_unsafe{false};

    static Config LoadFromEnvironment();
    std::string EffectiveJson() const;
};

}  // namespace relational_db

#endif  // CONFIG_H
