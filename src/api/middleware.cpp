#include <algorithm>
#include <cctype>
#include <charconv>
#include <iostream>
#include <string_view>

#include "internal.h"
#include "metrics/metrics.h"

namespace relational_db {
namespace {

bool IsApiJsonPost(const crow::request& request) {
    return request.method == crow::HTTPMethod::POST &&
           (request.url == "/api/query" || request.url == "/api/v1/query" ||
            request.url == "/api/v1/batch");
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

HttpRoute RouteMetric(std::string_view path) {
    if (path == "/api/v1/snapshot" || path == "/api/v1/catalog" ||
        path == "/api/v1/stats/history" || path == "/api/v1/slow-queries" ||
        path == "/api/v1/activity" ||
        path.rfind("/api/v1/inspect/", 0) == 0 ||
        (path.rfind("/api/v1/tables/", 0) == 0 && path.find("/rows") != std::string_view::npos)) {
        return HttpRoute::INSPECTOR;
    }
    if (path == "/health") {
        return HttpRoute::HEALTH;
    }
    if (path == "/api/query") {
        return HttpRoute::LEGACY_QUERY;
    }
    if (path == "/api/v1/query") {
        return HttpRoute::QUERY;
    }
    if (path == "/api/metrics") {
        return HttpRoute::API_METRICS;
    }
    if (path == "/metrics") {
        return HttpRoute::PROMETHEUS;
    }
    if (path == "/api/v1/stats") {
        return HttpRoute::STATS;
    }
    if (path == "/api/v1/stats/buffer_pool") {
        return HttpRoute::BUFFER_POOL_STATS;
    }
    if (path == "/api/v1/stats/storage") {
        return HttpRoute::STORAGE_STATS;
    }
    if (path == "/api/v1/batch") {
        return HttpRoute::BATCH;
    }
    return HttpRoute::OTHER;
}

bool IsInspectorRoute(std::string_view path) {
    return RouteMetric(path) == HttpRoute::INSPECTOR;
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

bool IsTrustedHttps(const crow::request& request, uint64_t trusted_hops) {
    if (trusted_hops == 0U) {
        return false;
    }
    const std::string forwarded_proto = request.get_header_value("X-Forwarded-Proto");
    if (forwarded_proto.empty() || forwarded_proto.size() > 256U) {
        return false;
    }

    size_t end = forwarded_proto.size();
    for (uint64_t hop = 1U; hop < trusted_hops; ++hop) {
        const size_t separator = forwarded_proto.rfind(',', end - 1U);
        if (separator == std::string::npos) {
            return false;
        }
        end = separator;
    }
    const size_t separator = forwarded_proto.rfind(',', end - 1U);
    size_t begin = separator == std::string::npos ? 0U : separator + 1U;
    while (begin < end && std::isspace(static_cast<unsigned char>(forwarded_proto[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(forwarded_proto[end - 1U])) != 0) {
        --end;
    }
    return end - begin == 5U &&
           std::tolower(static_cast<unsigned char>(forwarded_proto[begin])) == 'h' &&
           std::tolower(static_cast<unsigned char>(forwarded_proto[begin + 1U])) == 't' &&
           std::tolower(static_cast<unsigned char>(forwarded_proto[begin + 2U])) == 't' &&
           std::tolower(static_cast<unsigned char>(forwarded_proto[begin + 3U])) == 'p' &&
           std::tolower(static_cast<unsigned char>(forwarded_proto[begin + 4U])) == 's';
}

}  // namespace

bool ApiMiddleware::ConsumeToken(const std::string& client_ip, bool inspect) {
    constexpr double kInspectRate = 5.0;
    constexpr double kInspectBurst = 10.0;
    const std::string bucket_key = inspect ? "inspect:" + client_ip : client_ip;
    const double rate = inspect ? kInspectRate : config.rate_limit_rps;
    const double burst = inspect ? kInspectBurst : config.rate_limit_burst;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(buckets_mutex_);
    auto iterator = buckets_.find(bucket_key);
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
        iterator = buckets_.emplace(bucket_key, Bucket{burst, now}).first;
    }
    Bucket& bucket = iterator->second;
    const double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
    bucket.tokens = std::min(burst, bucket.tokens + elapsed * rate);
    bucket.updated = now;
    if (bucket.tokens < 1.0) {
        MetricsRegistry::Instance().rate_limited_requests.Increment();
        if (inspect) {
            MetricsRegistry::Instance().inspect_rate_limited_requests.Increment();
        }
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

bool ApiMiddleware::WaitForIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(active_requests_mutex_);
    return active_requests_changed_.wait_for(lock, timeout, [this]() {
        return active_requests.load(std::memory_order_acquire) == 0U;
    });
}

void ApiMiddleware::CompleteRequest(context& ctx) {
    if (!ctx.active_counted) {
        return;
    }
    ctx.active_counted = false;
    active_requests.fetch_sub(1, std::memory_order_acq_rel);
    active_requests_changed_.notify_all();
}

void ApiMiddleware::before_handle(crow::request& request, crow::response& response, context& ctx) {
    active_requests.fetch_add(1, std::memory_order_acq_rel);
    ctx.active_counted = true;
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

    if (draining.load(std::memory_order_acquire) && request.url != "/healthz" &&
        request.url != "/readyz") {
        response = ErrorResponse(503, "UNAVAILABLE", "Service is draining", ctx.request_id);
        complete_early();
        return;
    }

    if (request.method == crow::HTTPMethod::OPTIONS) {
        const std::string origin = request.get_header_value("Origin");
        const std::string method = request.get_header_value("Access-Control-Request-Method");
        if (!ConsumeToken(ctx.client_ip, IsInspectorRoute(request.url))) {
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

    const bool inspector_route = IsInspectorRoute(request.url);
    if (inspector_route && !config.inspect_enabled) {
        response =
            ErrorResponse(404, "NOT_FOUND", "Inspector endpoint is disabled", ctx.request_id);
        complete_early();
        return;
    }
    if (inspector_route && !config.api_key.empty() &&
        !ApiKeyMatches(config.api_key, request.get_header_value("X-API-Key"))) {
        response =
            ErrorResponse(401, "API_KEY_REQUIRED",
                          "A valid X-API-Key is required for inspector routes", ctx.request_id);
        complete_early();
        return;
    }

    if (!ConsumeToken(ctx.client_ip, inspector_route)) {
        response =
            ErrorResponse(429, "RATE_LIMITED", "Request rate limit exceeded", ctx.request_id);
        response.add_header("Retry-After", "1");
        complete_early();
    }
}

void ApiMiddleware::after_handle(crow::request& request, crow::response& response, context& ctx) {
    struct RequestCompletion {
        ApiMiddleware& middleware;
        context& request_context;
        ~RequestCompletion() {
            middleware.CompleteRequest(request_context);
        }
    } completion{*this, ctx};

    ApplyResponseHeaders(response, IsTrustedHttps(request, config.trust_proxy_hops));
    if (request.url.rfind("/api/", 0) == 0) {
        const auto cache_header = response.headers.find("Cache-Control");
        if (cache_header == response.headers.end()) {
            response.add_header("Cache-Control", "no-store");
        } else {
            cache_header->second = "no-store";
        }
    }
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
    MetricsRegistry& metrics = MetricsRegistry::Instance();
    metrics.RecordHttpRequest(RouteMetric(request.url), response.code);
    metrics.http_request_duration_us.Observe(duration);
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
