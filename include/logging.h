#ifndef RELDB_LOGGING_H
#define RELDB_LOGGING_H

#include <cstdio>
#include <string>

namespace relational_db {

inline std::string EscapeLogJson(const std::string& value) {
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

inline void LogMessage(const std::string& level, const std::string& event,
                       const std::string& message) {
    const std::string line = "{\"level\":\"" + EscapeLogJson(level) + "\",\"event\":\"" +
                             EscapeLogJson(event) + "\",\"message\":\"" +
                             EscapeLogJson(message) + "\"}\n";
    (void)std::fwrite(line.data(), 1U, line.size(), stderr);
    (void)std::fflush(stderr);
}

inline void LogJson(const std::string& level, const std::string& event,
                    const std::string& fields_json) {
    const std::string line = "{\"level\":\"" + EscapeLogJson(level) + "\",\"event\":\"" +
                             EscapeLogJson(event) + "\",\"fields\":" + fields_json + "}\n";
    (void)std::fwrite(line.data(), 1U, line.size(), stderr);
    (void)std::fflush(stderr);
}

}  // namespace relational_db

#endif
