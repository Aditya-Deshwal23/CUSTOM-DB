#ifndef DB_ERROR_H
#define DB_ERROR_H

#include <stdexcept>
#include <string>

#include "config.h"

namespace relational_db {

enum class ErrorCode {
    INVALID_DB_FILE,
    UNSUPPORTED_VERSION,
    HEADER_CORRUPT,
    PAGE_CHECKSUM_MISMATCH,
    PAGE_OUT_OF_RANGE,
    BUFFER_POOL_EXHAUSTED,
    INVALID_ARGUMENT,
    IO_ERROR
};

class DbError : public std::runtime_error {
   public:
    DbError(ErrorCode code, const std::string& message, page_id_t page_id = INVALID_PAGE_ID)
        : std::runtime_error(message), code_(code), page_id_(page_id) {}

    ErrorCode Code() const noexcept {
        return code_;
    }
    page_id_t PageId() const noexcept {
        return page_id_;
    }

   private:
    ErrorCode code_;
    page_id_t page_id_;
};

}  // namespace relational_db

#endif
