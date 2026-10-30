#ifndef FAULT_INJECTING_DISK_MANAGER_H
#define FAULT_INJECTING_DISK_MANAGER_H

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "crc32c.h"
#include "db_error.h"
#include "disk_manager.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace relational_db::testing {

class FaultInjectingDiskManager final : public IDiskManager {
   public:
    FaultInjectingDiskManager(IDiskManager& inner, std::string path)
        : inner_(inner), path_(std::move(path)) {}

    void FailNthWrite(size_t nth) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_on_write_ = nth;
        write_count_ = 0;
    }

    void TearNextWrite(size_t prefix_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        torn_prefix_bytes_ = prefix_bytes;
    }

    void DropSync(bool enabled = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        drop_sync_ = enabled;
    }

    void FlipReadBit(size_t byte_offset, uint8_t bit_mask = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        flip_byte_offset_ = byte_offset;
        flip_bit_mask_ = bit_mask;
    }

    void InjectEnospcOnWrite(bool enabled = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        enospc_ = enabled;
    }

    void ReadPage(page_id_t page_id, char* page_data) override {
        inner_.ReadPage(page_id, page_data);
        std::lock_guard<std::mutex> lock(mutex_);
        if (flip_byte_offset_ && *flip_byte_offset_ < PAGE_DATA_SIZE) {
            page_data[*flip_byte_offset_] =
                static_cast<char>(page_data[*flip_byte_offset_] ^ flip_bit_mask_);
            flip_byte_offset_.reset();
        }
    }

    void WritePage(page_id_t page_id, const char* page_data, PageType type = PageType::HEAP,
                   uint16_t flags = 0, uint64_t lsn = 0) override {
        std::unique_lock<std::mutex> lock(mutex_);
        ++write_count_;
        if (enospc_) {
            throw DbError(ErrorCode::IO_ERROR, "Injected ENOSPC");
        }
        if (fail_on_write_ != 0 && write_count_ == fail_on_write_) {
            throw DbError(ErrorCode::IO_ERROR, "Injected write failure");
        }
        if (torn_prefix_bytes_) {
            const size_t prefix_bytes = *torn_prefix_bytes_;
            torn_prefix_bytes_.reset();
            if (prefix_bytes > PAGE_SIZE) {
                throw DbError(ErrorCode::IO_ERROR, "Torn-write prefix exceeds page size");
            }
            lock.unlock();
            WriteRawPrefix(page_id, page_data, prefix_bytes, type, flags, lsn);
            throw DbError(ErrorCode::IO_ERROR, "Injected torn write");
        }
        lock.unlock();
        inner_.WritePage(page_id, page_data, type, flags, lsn);
    }

    void WriteHeaderPage(const char* page_data) override {
        std::unique_lock<std::mutex> lock(mutex_);
        ++write_count_;
        if (enospc_) {
            throw DbError(ErrorCode::IO_ERROR, "Injected ENOSPC");
        }
        if (fail_on_write_ != 0 && write_count_ == fail_on_write_) {
            throw DbError(ErrorCode::IO_ERROR, "Injected write failure");
        }
        lock.unlock();
        inner_.WriteHeaderPage(page_data);
    }

    void Sync() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!drop_sync_) {
            inner_.Sync();
        }
    }

    uint64_t SizeInPages() const override {
        return inner_.SizeInPages();
    }
    void Extend(uint64_t page_count) override {
        inner_.Extend(page_count);
    }
    DiskStats Stats() const override {
        return inner_.Stats();
    }

   private:
    static void PutU16(uint8_t* bytes, uint16_t value) {
        bytes[0] = static_cast<uint8_t>(value);
        bytes[1] = static_cast<uint8_t>(value >> 8U);
    }

    static void PutU32(uint8_t* bytes, uint32_t value) {
        for (size_t index = 0; index < 4; ++index) {
            bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
        }
    }

    static void PutU64(uint8_t* bytes, uint64_t value) {
        for (size_t index = 0; index < 8; ++index) {
            bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
        }
    }

    void WriteRawPrefix(page_id_t page_id, const char* page_data, size_t raw_prefix_bytes,
                        PageType type, uint16_t flags, uint64_t lsn) {
        std::array<uint8_t, PAGE_SIZE> image{};
        PutU16(image.data() + 4, static_cast<uint16_t>(type));
        PutU16(image.data() + 6, flags);
        PutU64(image.data() + 8, lsn);
        std::memcpy(image.data() + PAGE_HEADER_SIZE, page_data, PAGE_DATA_SIZE);
        PutU32(image.data(), Crc32c(image.data() + 4, PAGE_SIZE - 4));
#ifdef _WIN32
        const int descriptor = _open(path_.c_str(), _O_WRONLY | _O_BINARY);
        if (descriptor < 0 ||
            _lseeki64(descriptor, static_cast<__int64>(page_id) * PAGE_SIZE, SEEK_SET) < 0) {
            if (descriptor >= 0) {
                _close(descriptor);
            }
            throw DbError(ErrorCode::IO_ERROR, "Unable to open file for torn-write injection");
        }
        size_t written = 0;
        while (written < raw_prefix_bytes) {
            const int count = _write(descriptor, image.data() + written,
                                     static_cast<unsigned int>(raw_prefix_bytes - written));
            if (count <= 0) {
                _close(descriptor);
                throw DbError(ErrorCode::IO_ERROR, "Unable to persist torn-write prefix");
            }
            written += static_cast<size_t>(count);
        }
        if (_commit(descriptor) != 0) {
            _close(descriptor);
            throw DbError(ErrorCode::IO_ERROR, "Unable to persist torn-write prefix");
        }
        _close(descriptor);
#else
        const int descriptor = open(path_.c_str(), O_WRONLY);
        if (descriptor < 0) {
            throw DbError(ErrorCode::IO_ERROR, "Unable to open file for torn-write injection");
        }
        size_t written = 0;
        while (written < raw_prefix_bytes) {
            const ssize_t count =
                pwrite(descriptor, image.data() + written, raw_prefix_bytes - written,
                       static_cast<off_t>(static_cast<uint64_t>(page_id) * PAGE_SIZE + written));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                close(descriptor);
                throw DbError(ErrorCode::IO_ERROR, "Unable to persist torn-write prefix");
            }
            written += static_cast<size_t>(count);
        }
        if (fsync(descriptor) != 0) {
            close(descriptor);
            throw DbError(ErrorCode::IO_ERROR, "Unable to persist torn-write prefix");
        }
        close(descriptor);
#endif
    }

    IDiskManager& inner_;
    std::string path_;
    mutable std::mutex mutex_;
    size_t fail_on_write_{0};
    size_t write_count_{0};
    bool drop_sync_{false};
    bool enospc_{false};
    std::optional<size_t> torn_prefix_bytes_;
    std::optional<size_t> flip_byte_offset_;
    uint8_t flip_bit_mask_{1};
};

}  // namespace relational_db::testing

#endif
