#ifndef PAGE_GUARD_H
#define PAGE_GUARD_H

#include "page.h"
#include <memory>
#include <shared_mutex>
#include <stdexcept>

namespace relational_db {

/**
 * @brief RAII guard for safely managing page access with latch crabbing
 */
class PageGuard {
public:
    static PageGuard ReadLock(Page* page) {
        if (!page) throw std::invalid_argument("Page cannot be nullptr");
        page->RLock(); 
        auto guard = PageGuard(page);
        guard.read_locked_ = true;
        return guard;
    }

    static PageGuard WriteLock(Page* page) {
        if (!page) throw std::invalid_argument("Page cannot be nullptr");
        page->WLock(); 
        auto guard = PageGuard(page);
        guard.write_locked_ = true;
        return guard;
    }

    PageGuard(PageGuard&& other) noexcept 
        : page_(other.page_), read_locked_(other.read_locked_), write_locked_(other.write_locked_) {
        other.page_ = nullptr;
        other.read_locked_ = false;
        other.write_locked_ = false;
    }

    PageGuard& operator=(PageGuard&& other) noexcept {
        if (this != &other) {
            Unlock();
            page_ = other.page_;
            read_locked_ = other.read_locked_;
            write_locked_ = other.write_locked_;
            
            other.page_ = nullptr;
            other.read_locked_ = false;
            other.write_locked_ = false;
        }
        return *this;
    }

    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;

    ~PageGuard() {
        Unlock();
    }

    Page* operator->() const { return page_; }
    Page& operator*() const { return *page_; }
    Page* Get() const { return page_; }

    void Unlock() {
        if (!page_) return;
        if (read_locked_)  { page_->RUnlock(); }
        if (write_locked_) { page_->WUnlock(); }
        page_ = nullptr;
        read_locked_ = false;
        write_locked_ = false;
    }

    bool IsLocked() const { return page_ != nullptr; }
    bool IsReadLocked() const { return read_locked_; }
    bool IsWriteLocked() const { return write_locked_; }

    Page* Release() {
        Page* p = page_;
        page_ = nullptr;
        read_locked_ = false;
        write_locked_ = false;
        return p;
    }

private:
    explicit PageGuard(Page* page) : page_(page), read_locked_(false), write_locked_(false) {}

    Page* page_;
    bool read_locked_;
    bool write_locked_;
};

class ReadPageGuard : public PageGuard {
public:
    explicit ReadPageGuard(Page* page) : PageGuard(ReadLock(page)) {}
};

class WritePageGuard : public PageGuard {
public:
    explicit WritePageGuard(Page* page) : PageGuard(WriteLock(page)) {}
};

} // namespace relational_db

#endif // PAGE_GUARD_H
