#ifndef PAGE_GUARD_H
#define PAGE_GUARD_H

#include "db_error.h"
#include "page.h"

#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>

namespace relational_db {

enum class GuardMode { BASIC, READ, WRITE };

class PageGuard {
    friend class BufferPoolManager;

   public:
    using ReleaseFunction = void (*)(void*, Frame*, bool, std::thread::id) noexcept;
    using MutationFunction = void (*)(void*, Frame*);

    PageGuard() = default;
    PageGuard(Frame* frame, frame_id_t frame_id, void* owner, ReleaseFunction release,
              GuardMode mode, MutationFunction mutation = nullptr)
        : frame_(frame),
          frame_id_(frame_id),
          owner_(owner),
          release_(release),
          mode_(mode),
          mutation_(mutation),
          origin_thread_(std::this_thread::get_id()) {}

    PageGuard(PageGuard&& other) noexcept {
        MoveFrom(std::move(other));
    }

    PageGuard& operator=(PageGuard&& other) noexcept {
        if (this != &other) {
            Drop();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    ~PageGuard() {
        Drop();
    }

    explicit operator bool() const noexcept {
        return frame_ != nullptr;
    }

    page_id_t GetPageId() const {
        EnsureValid();
        return frame_->page_id;
    }

    PageType GetPageType() const {
        EnsureValid();
        return frame_->page_type;
    }

    frame_id_t GetFrameId() const {
        EnsureValid();
        return frame_id_;
    }

    template <typename T>
    const T* As() const {
        CheckType<T>();
        EnsureValid();
        CheckAlignment<T>();
        return reinterpret_cast<const T*>(frame_->data.data());
    }

    template <typename T>
    T* AsMut() {
        CheckType<T>();
        EnsureValid();
        if (mode_ == GuardMode::READ) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Cannot mutate a read-guarded page",
                          frame_->page_id);
        }
        CheckAlignment<T>();
        RegisterMutation();
        dirty_on_release_ = true;
        return reinterpret_cast<T*>(frame_->data.data());
    }

    void MarkDirty() {
        EnsureValid();
        if (mode_ == GuardMode::READ) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Cannot dirty a read-guarded page",
                          frame_->page_id);
        }
        dirty_on_release_ = true;
    }

    void SetPageType(PageType type) {
        EnsureValid();
        if (mode_ == GuardMode::READ) {
            throw DbError(ErrorCode::INVALID_ARGUMENT,
                          "Cannot change the type of a read-guarded page", frame_->page_id);
        }
        RegisterMutation();
        frame_->page_type = type;
        dirty_on_release_ = true;
    }

    bool IsValid() const noexcept {
        return frame_ != nullptr;
    }

    void Drop() noexcept {
        if (frame_ == nullptr) {
            return;
        }
        Frame* frame = frame_;
        void* owner = owner_;
        ReleaseFunction release = release_;
        const GuardMode mode = mode_;
        const std::thread::id origin_thread = origin_thread_;
        const bool dirty = dirty_on_release_ || mode == GuardMode::WRITE;
        frame_ = nullptr;
        frame_id_ = -1;
        owner_ = nullptr;
        release_ = nullptr;
        mutation_ = nullptr;
        dirty_on_release_ = false;
        if (mode == GuardMode::READ) {
            frame->rwlatch.unlock_shared();
        } else if (mode == GuardMode::WRITE) {
            frame->rwlatch.unlock();
        }
        if (release != nullptr) {
            release(owner, frame, dirty, origin_thread);
        }
    }

   private:
    template <typename T>
    static constexpr void CheckType() {
        static_assert(sizeof(T) <= PAGE_SIZE, "Typed page view exceeds PAGE_SIZE");
    }

    template <typename T>
    void CheckAlignment() const {
        if (reinterpret_cast<std::uintptr_t>(frame_->data.data()) % alignof(T) != 0) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Typed page view is misaligned",
                          frame_->page_id);
        }
    }

    void EnsureValid() const {
        if (frame_ == nullptr) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Page guard is empty");
        }
    }

    void RegisterMutation() {
        if (mutation_ != nullptr) {
            mutation_(owner_, frame_);
        }
    }

    void MoveFrom(PageGuard&& other) noexcept {
        frame_ = other.frame_;
        frame_id_ = other.frame_id_;
        owner_ = other.owner_;
        release_ = other.release_;
        mode_ = other.mode_;
        mutation_ = other.mutation_;
        dirty_on_release_ = other.dirty_on_release_;
        origin_thread_ = other.origin_thread_;
        other.frame_ = nullptr;
        other.frame_id_ = -1;
        other.owner_ = nullptr;
        other.release_ = nullptr;
        other.mutation_ = nullptr;
        other.dirty_on_release_ = false;
    }

    Frame* FramePointer() const noexcept {
        return frame_;
    }

    void Disarm() noexcept {
        if (frame_ == nullptr) {
            return;
        }
        frame_->rwlatch.unlock();
        frame_ = nullptr;
        frame_id_ = -1;
        owner_ = nullptr;
        release_ = nullptr;
        mutation_ = nullptr;
        dirty_on_release_ = false;
    }

    Frame* frame_{nullptr};
    frame_id_t frame_id_{-1};
    void* owner_{nullptr};
    ReleaseFunction release_{nullptr};
    GuardMode mode_{GuardMode::BASIC};
    MutationFunction mutation_{nullptr};
    bool dirty_on_release_{false};
    std::thread::id origin_thread_{};
};

using BasicPageGuard = PageGuard;
using ReadPageGuard = PageGuard;
using WritePageGuard = PageGuard;

}  // namespace relational_db

#endif
