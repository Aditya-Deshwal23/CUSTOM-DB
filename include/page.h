#ifndef PAGE_H
#define PAGE_H

#include "config.h"
#include "disk_manager.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <shared_mutex>

namespace relational_db {

enum class IoState { READY, LOADING, WRITING_BACK };

struct Frame {
    alignas(PAGE_SIZE) std::array<char, PAGE_DATA_SIZE> data{};
    std::atomic<int> pin_count{0};
    std::atomic<bool> dirty{false};
    page_id_t page_id{INVALID_PAGE_ID};
    PageType page_type{PageType::HEAP};
    std::shared_mutex rwlatch;
    IoState io_state{IoState::READY};
    std::condition_variable condition;
    uint64_t lsn{0};

    Frame() = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    Frame(Frame&& other) noexcept
        : data(other.data),
          pin_count(other.pin_count.load(std::memory_order_relaxed)),
          dirty(other.dirty.load(std::memory_order_relaxed)),
          page_id(other.page_id),
          page_type(other.page_type),
          io_state(other.io_state),
          lsn(other.lsn) {}
    Frame& operator=(Frame&&) = delete;
};

}  // namespace relational_db

#endif
