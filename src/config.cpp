#include "config.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace relational_db {
namespace {

std::string EnvironmentValue(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

uint64_t ReadUnsigned(const char* name, uint64_t fallback, bool allow_zero = false) {
    const std::string text = EnvironmentValue(name);
    if (text.empty()) {
        return fallback;
    }

    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        (!allow_zero && value == 0)) {
        throw std::invalid_argument(std::string(name) +
                                    (allow_zero ? " must be a non-negative integer"
                                                : " must be a positive integer"));
    }
    return value;
}

bool ReadBoolean(const char* name, bool fallback) {
    const std::string value = EnvironmentValue(name);
    if (value.empty()) {
        return fallback;
    }
    if (value == "1") {
        return true;
    }
    if (value == "0") {
        return false;
    }
    throw std::invalid_argument(std::string(name) + " must be 0 or 1");
}

double ReadPositiveDouble(const char* name, double fallback) {
    const std::string text = EnvironmentValue(name);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value) || value <= 0.0) {
        throw std::invalid_argument(std::string(name) + " must be a positive number");
    }
    return value;
}

std::vector<std::string> ReadOrigins() {
    const std::string value = EnvironmentValue("CORS_ORIGINS");
    std::vector<std::string> origins;
    size_t begin = 0;
    while (begin < value.size()) {
        size_t end = value.find(',', begin);
        if (end == std::string::npos) {
            end = value.size();
        }
        size_t first = begin;
        size_t last = end;
        while (first < last && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
            ++first;
        }
        while (last > first &&
               std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0) {
            --last;
        }
        if (first != last) {
            std::string origin = value.substr(first, last - first);
            const size_t separator = origin.find("://");
            const std::string_view scheme(
                origin.data(), separator == std::string::npos ? origin.size() : separator);
            if (separator == std::string::npos ||
                (scheme != "http" && scheme != "https") || separator + 3U >= origin.size() ||
                origin.find_first_of("/?#", separator + 3U) != std::string::npos) {
                throw std::invalid_argument(
                    "CORS_ORIGINS must contain HTTP(S) origins without paths");
            }
            origins.push_back(std::move(origin));
        }
        begin = end + 1U;
    }
    return origins;
}

std::string EscapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2U);
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (character < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += hex[(character >> 4U) & 0x0fU];
                    escaped += hex[character & 0x0fU];
                } else {
                    escaped += static_cast<char>(character);
                }
                break;
        }
    }
    return escaped;
}

}  // namespace

Config Config::LoadFromEnvironment() {
    Config config;
    const std::string port = EnvironmentValue("PORT");
    if (!port.empty()) {
        uint64_t value = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || value == 0U ||
            value > std::numeric_limits<uint16_t>::max()) {
            throw std::invalid_argument("PORT must be an integer between 1 and 65535");
        }
        config.port = static_cast<uint16_t>(value);
    }

    const std::string db_path = EnvironmentValue("DB_PATH");
    if (!db_path.empty()) {
        config.db_path = db_path;
    }
    config.wal_path = EnvironmentValue("WAL_PATH");
    if (config.wal_path.empty()) {
        config.wal_path = config.db_path + "-wal";
    }
    if (config.wal_path == config.db_path) {
        throw std::invalid_argument("WAL_PATH must differ from DB_PATH");
    }

    config.buffer_pool_frames = ReadUnsigned("BUFFER_POOL_FRAMES", config.buffer_pool_frames);
    if (config.buffer_pool_frames < 16U ||
        config.buffer_pool_frames >
            static_cast<uint64_t>(std::numeric_limits<frame_id_t>::max())) {
        throw std::invalid_argument("BUFFER_POOL_FRAMES must be between 16 and " +
                                    std::to_string(std::numeric_limits<frame_id_t>::max()));
    }
    const std::string buffer_pool_policy = EnvironmentValue("BUFFER_POOL_POLICY");
    if (!buffer_pool_policy.empty()) {
        config.buffer_pool_policy = buffer_pool_policy;
    }
    if (config.buffer_pool_policy != "clock" && config.buffer_pool_policy != "lruk") {
        throw std::invalid_argument("BUFFER_POOL_POLICY must be either 'clock' or 'lruk'");
    }

    const std::string durability = EnvironmentValue("DURABILITY");
    if (!durability.empty()) {
        config.durability = durability;
    }
    if (config.durability != "full" && config.durability != "normal" &&
        config.durability != "off") {
        throw std::invalid_argument("DURABILITY must be full, normal, or off");
    }
    config.wal_max_bytes = ReadUnsigned("WAL_MAX_BYTES", config.wal_max_bytes);
    config.wal_sync_interval_ms =
        ReadUnsigned("WAL_SYNC_INTERVAL_MS", config.wal_sync_interval_ms);
    config.read_only = ReadBoolean("READ_ONLY", config.read_only);
    config.api_key = EnvironmentValue("API_KEY");
    if (config.api_key.size() > 4096U) {
        throw std::invalid_argument("API_KEY must not exceed 4096 bytes");
    }
    config.cors_origins = ReadOrigins();

    config.max_body_bytes = ReadUnsigned("MAX_BODY_BYTES", config.max_body_bytes);
    config.max_sql_bytes = ReadUnsigned("MAX_SQL_BYTES", config.max_sql_bytes);
    config.max_result_rows = ReadUnsigned("MAX_RESULT_ROWS", config.max_result_rows);
    config.statement_timeout_ms =
        ReadUnsigned("STATEMENT_TIMEOUT_MS", config.statement_timeout_ms);
    config.rate_limit_rps = ReadPositiveDouble("RATE_LIMIT_RPS", config.rate_limit_rps);
    config.rate_limit_burst = ReadPositiveDouble("RATE_LIMIT_BURST", config.rate_limit_burst);
    config.max_db_bytes = ReadUnsigned("MAX_DB_BYTES", config.max_db_bytes, true);

    const std::string log_level = EnvironmentValue("LOG_LEVEL");
    if (!log_level.empty()) {
        config.log_level = log_level;
    }
    if (config.log_level != "debug" && config.log_level != "info" &&
        config.log_level != "warn" && config.log_level != "error" &&
        config.log_level != "off") {
        throw std::invalid_argument("LOG_LEVEL must be debug, info, warn, error, or off");
    }
    config.demo_load = ReadBoolean("DEMO_LOAD", config.demo_load);
    config.slow_query_ms = ReadUnsigned("SLOW_QUERY_MS", config.slow_query_ms, true);
    config.drain_timeout_ms = ReadUnsigned("DRAIN_TIMEOUT_MS", config.drain_timeout_ms);
    config.allow_unsafe = ReadBoolean("ALLOW_UNSAFE", config.allow_unsafe);

    if (config.durability == "off" && !config.allow_unsafe) {
        throw std::invalid_argument("DURABILITY=off requires ALLOW_UNSAFE=1");
    }
    if (config.max_body_bytes > 16U * 1024U * 1024U ||
        config.max_sql_bytes > 16U * 1024U * 1024U ||
        config.max_result_rows > MAX_RESULT_ROWS || config.statement_timeout_ms > 600000U ||
        config.wal_sync_interval_ms > 600000U || config.slow_query_ms > 3600000U ||
        config.rate_limit_rps > 1000000.0 ||
        config.rate_limit_burst < 1.0 || config.rate_limit_burst > 1000000.0 ||
        config.drain_timeout_ms > 600000U) {
        throw std::invalid_argument("Configured limits are outside their supported ranges");
    }
    return config;
}

std::string Config::EffectiveJson() const {
    return "{\"PORT\":" + std::to_string(port) + ",\"DB_PATH\":\"" + EscapeJson(db_path) +
           "\",\"WAL_PATH\":\"" + EscapeJson(wal_path) +
           "\",\"BUFFER_POOL_FRAMES\":" + std::to_string(buffer_pool_frames) +
           ",\"BUFFER_POOL_POLICY\":\"" + EscapeJson(buffer_pool_policy) +
           "\",\"DURABILITY\":\"" + EscapeJson(durability) +
           "\",\"WAL_MAX_BYTES\":" + std::to_string(wal_max_bytes) +
           ",\"WAL_SYNC_INTERVAL_MS\":" + std::to_string(wal_sync_interval_ms) +
           ",\"READ_ONLY\":" + (read_only ? "true" : "false") +
           ",\"API_KEY\":\"" + (api_key.empty() ? "" : "[redacted]") +
           "\",\"CORS_ORIGINS\":" + [&]() {
               std::string json = "[";
               for (size_t index = 0; index < cors_origins.size(); ++index) {
                   if (index != 0U) {
                       json += ',';
                   }
                   json += "\"" + EscapeJson(cors_origins[index]) + "\"";
               }
               return json + "]";
           }() +
           ",\"MAX_BODY_BYTES\":" + std::to_string(max_body_bytes) +
           ",\"MAX_SQL_BYTES\":" + std::to_string(max_sql_bytes) +
           ",\"MAX_RESULT_ROWS\":" + std::to_string(max_result_rows) +
           ",\"STATEMENT_TIMEOUT_MS\":" + std::to_string(statement_timeout_ms) +
           ",\"RATE_LIMIT_RPS\":" + std::to_string(rate_limit_rps) +
           ",\"RATE_LIMIT_BURST\":" + std::to_string(rate_limit_burst) +
           ",\"MAX_DB_BYTES\":" + std::to_string(max_db_bytes) + ",\"LOG_LEVEL\":\"" +
           EscapeJson(log_level) + "\",\"DEMO_LOAD\":" + (demo_load ? "true" : "false") +
           ",\"SLOW_QUERY_MS\":" + std::to_string(slow_query_ms) +
           ",\"DRAIN_TIMEOUT_MS\":" + std::to_string(drain_timeout_ms) +
           ",\"ALLOW_UNSAFE\":" + (allow_unsafe ? "true" : "false") + "}";
}

}  // namespace relational_db
