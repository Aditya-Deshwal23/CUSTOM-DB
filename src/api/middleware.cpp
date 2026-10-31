#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "internal.h"

namespace relational_db {
namespace {

bool IsApiJsonPost(const crow::request& request) {
    return request.method == crow::HTTPMethod::POST &&
           (request.url == "/api/query" || request.url == "/api/v1/query");
}

bool IsAllowedOrigin(const ApiConfig& config, const std::string& origin) {
    return std::find(config.cors_origins.begin(), config.cors_origins.end(), origin) !=
           config.cors_origins.end();
}

bool IsSafeRequestId(const std::string& value) {
    return !value.empty() && value.size() <= 128U &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_' ||
                      character == '.';
           });
}

std::string MethodName(crow::HTTPMethod method) {
    switch (method) {
        case crow::HTTPMethod::GET:
            return "GET";
        case crow::HTTPMethod::POST:
            return "POST";
        case crow::HTTPMethod::PUT:
            return "PUT";
        case crow::HTTPMethod::DELETE:
            return "DELETE";
        case crow::HTTPMethod::PATCH:
            return "PATCH";
        case crow::HTTPMethod::OPTIONS:
            return "OPTIONS";
        case crow::HTTPMethod::HEAD:
            return "HEAD";
        default:
            return "UNKNOWN";
    }
}

std::string ReadEnvironment(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

uint64_t ReadUnsigned(const char* name, uint64_t fallback) {
    const std::string text = ReadEnvironment(name);
    if (text.empty()) {
        return fallback;
    }
    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0U) {
        throw std::invalid_argument(std::string("Invalid positive integer in ") + name);
    }
    return value;
}

double ReadPositiveDouble(const char* name, double fallback) {
    const std::string text = ReadEnvironment(name);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value) || value <= 0.0) {
        throw std::invalid_argument(std::string("Invalid positive number in ") + name);
    }
    return value;
}

bool ReadBoolean(const char* name, bool fallback) {
    const std::string text = ReadEnvironment(name);
    if (text.empty()) {
        return fallback;
    }
    if (text == "1") {
        return true;
    }
    if (text == "0") {
        return false;
    }
    throw std::invalid_argument(std::string(name) + " must be 0 or 1");
}

std::vector<std::string> ReadOrigins() {
    const std::string value = ReadEnvironment("CORS_ORIGINS");
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
        while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0) {
            --last;
        }
        if (last > first) {
            std::string origin = value.substr(first, last - first);
            const size_t separator = origin.find("://");
            const std::string_view scheme(origin.data(),
                                          separator == std::string::npos ? origin.size() : separator);
            if (origin == "*" || separator == std::string::npos ||
                (scheme != "http" && scheme != "https") ||
                separator + 3U >= origin.size() ||
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

}  // namespace

ApiConfig LoadApiConfig() {
    ApiConfig config;
    config.max_body_bytes = ReadUnsigned("MAX_BODY_BYTES", config.max_body_bytes);
    config.max_sql_bytes = ReadUnsigned("MAX_SQL_BYTES", config.max_sql_bytes);
    config.max_result_rows = ReadUnsigned("MAX_RESULT_ROWS", config.max_result_rows);
    config.statement_timeout_ms = ReadUnsigned("STATEMENT_TIMEOUT_MS", config.statement_timeout_ms);
    config.rate_limit_rps = ReadPositiveDouble("RATE_LIMIT_RPS", config.rate_limit_rps);
    config.rate_limit_burst = ReadPositiveDouble("RATE_LIMIT_BURST", config.rate_limit_burst);
    const std::string quota = ReadEnvironment("MAX_DB_BYTES");
    if (!quota.empty()) {
        uint64_t value = 0;
        const auto parsed = std::from_chars(quota.data(), quota.data() + quota.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != quota.data() + quota.size() || value == 0U) {
            throw std::invalid_argument("MAX_DB_BYTES must be a positive integer");
        }
        config.max_db_bytes = value;
    }
    config.read_only = ReadBoolean("READ_ONLY", false);
    config.api_key = ReadEnvironment("API_KEY");
    if (config.api_key.size() > 4096U) {
        throw std::invalid_argument("API_KEY must not exceed 4096 bytes");
    }
    config.cors_origins = ReadOrigins();
    config.log_level = ReadEnvironment("LOG_LEVEL");
    if (config.log_level.empty()) {
        config.log_level = "info";
    }
    if (config.log_level != "debug" && config.log_level != "info" && config.log_level != "warn" &&
        config.log_level != "error" && config.log_level != "off") {
        throw std::invalid_argument("LOG_LEVEL must be debug, info, warn, error, or off");
    }
    if (config.max_body_bytes > 16U * 1024U * 1024U || config.max_result_rows > MAX_RESULT_ROWS ||
        config.statement_timeout_ms > 600000U || config.rate_limit_burst < 1.0) {
        throw std::invalid_argument("API limits are outside their supported ranges");
    }
    return config;
}

bool ApiMiddleware::ConsumeToken(const std::string& client_ip) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(buckets_mutex_);
    auto iterator = buckets_.find(client_ip);
    if (iterator == buckets_.end()) {
        if (buckets_.size() >= 4096U) {
            const auto oldest = std::min_element(
                buckets_.begin(), buckets_.end(), [](const auto& left, const auto& right) {
                    return left.second.updated < right.second.updated;
                });
            if (oldest != buckets_.end()) {
                buckets_.erase(oldest);
            }
        }
        iterator = buckets_.emplace(client_ip, Bucket{config.rate_limit_burst, now}).first;
    }
    Bucket& bucket = iterator->second;
    const double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
    bucket.tokens =
        std::min(config.rate_limit_burst, bucket.tokens + elapsed * config.rate_limit_rps);
    bucket.updated = now;
    if (bucket.tokens < 1.0) {
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

void ApiMiddleware::before_handle(crow::request& request, crow::response& response, context& ctx) {
    const auto complete_early = [&response]() { response.end(); };
    ctx.started = std::chrono::steady_clock::now();
    ctx.client_ip = request.remote_ip_address.empty() ? "unknown" : request.remote_ip_address;
    const std::string provided_id = request.get_header_value("X-Request-Id");
    if (IsSafeRequestId(provided_id)) {
        ctx.request_id = provided_id;
    } else {
        ctx.request_id =
            "r" + std::to_string(next_request_id_.fetch_add(1, std::memory_order_relaxed));
    }

    if (request.method == crow::HTTPMethod::OPTIONS) {
        const std::string origin = request.get_header_value("Origin");
        const std::string method = request.get_header_value("Access-Control-Request-Method");
        if (!ConsumeToken(ctx.client_ip)) {
            response =
                ErrorResponse(429, "RATE_LIMITED", "Request rate limit exceeded", ctx.request_id);
            response.add_header("Retry-After", "1");
        } else if (!IsAllowedOrigin(config, origin) ||
                   (method != "GET" && method != "POST" && method != "OPTIONS")) {
            response = ErrorResponse(403, "CORS_ORIGIN_DENIED", "CORS preflight is not allowed",
                                     ctx.request_id);
        } else {
            response.code = 204;
            response.add_header("Access-Control-Allow-Origin", origin);
            response.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.add_header("Access-Control-Allow-Headers",
                                "Content-Type, X-API-Key, X-Request-Id");
            response.add_header("Access-Control-Max-Age", "600");
            response.add_header("Vary", "Origin");
        }
        complete_early();
        return;
    }

    const std::string content_length = request.get_header_value("Content-Length");
    if (!content_length.empty()) {
        uint64_t length = 0;
        const auto parsed = std::from_chars(content_length.data(),
                                            content_length.data() + content_length.size(), length);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != content_length.data() + content_length.size()) {
            response = ErrorResponse(400, "INVALID_CONTENT_LENGTH", "Invalid Content-Length",
                                     ctx.request_id);
            complete_early();
            return;
        }
        if (length > config.max_body_bytes) {
            response = ErrorResponse(413, "BODY_TOO_LARGE", "Request body exceeds MAX_BODY_BYTES",
                                     ctx.request_id);
            complete_early();
            return;
        }
    }
    if (request.body.size() > config.max_body_bytes) {
        response = ErrorResponse(413, "BODY_TOO_LARGE", "Request body exceeds MAX_BODY_BYTES",
                                 ctx.request_id);
        complete_early();
        return;
    }

    if (IsApiJsonPost(request)) {
        const std::string content_type = request.get_header_value("Content-Type");
        const size_t separator = content_type.find(';');
        const std::string_view media_type(
            content_type.data(), separator == std::string::npos ? content_type.size() : separator);
        if (media_type != "application/json") {
            response = ErrorResponse(415, "UNSUPPORTED_MEDIA_TYPE",
                                     "Content-Type must be application/json", ctx.request_id);
            complete_early();
            return;
        }
    }

    if (!ConsumeToken(ctx.client_ip)) {
        response =
            ErrorResponse(429, "RATE_LIMITED", "Request rate limit exceeded", ctx.request_id);
        response.add_header("Retry-After", "1");
        complete_early();
    }
}

void ApiMiddleware::after_handle(crow::request& request, crow::response& response, context& ctx) {
    ApplyResponseHeaders(response);
    response.add_header("X-Request-Id", ctx.request_id);
    const std::string origin = request.get_header_value("Origin");
    if (!origin.empty() && IsAllowedOrigin(config, origin)) {
        response.add_header("Access-Control-Allow-Origin", origin);
        response.add_header("Vary", "Origin");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - ctx.started)
                             .count();
    const uint64_t duration = static_cast<uint64_t>(std::max<int64_t>(elapsed, 0));
    requests.fetch_add(1, std::memory_order_relaxed);
    latency_us.fetch_add(duration, std::memory_order_relaxed);
    if (response.code >= 400) {
        errors.fetch_add(1, std::memory_order_relaxed);
    }
    const bool is_error = response.code >= 500;
    const bool is_warning = response.code >= 400 && response.code < 500;
    if (config.log_level == "off" || (config.log_level == "error" && !is_error) ||
        (config.log_level == "warn" && !is_error && !is_warning)) {
        return;
    }

    crow::json::wvalue log;
    log["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
    log["level"] = response.code >= 500 ? "error" : (response.code >= 400 ? "warn" : "info");
    log["request_id"] = ctx.request_id;
    log["method"] = MethodName(request.method);
    log["path"] = request.url;
    log["status"] = response.code;
    log["dur_ms"] = static_cast<double>(duration) / 1000.0;
    log["bytes"] = static_cast<uint64_t>(request.body.size());
    log["client_ip"] = ctx.client_ip;
    std::cout << log.dump() << '\n';
}

}  // namespace relational_db
