#ifndef DISK_MANAGER_H
#define DISK_MANAGER_H

#include "config.h"
#include <fstream>
#include <string>
#include <atomic>
#include <mutex>

namespace relational_db {

class DiskManager {
public:
    explicit DiskManager(const std::string& db_file);
    ~DiskManager();

    void WritePage(page_id_t page_id, const char* page_data);
    void ReadPage(page_id_t page_id, char* page_data);
    page_id_t AllocatePage();

    int GetNumFlushes() const { return num_flushes_; }
    int GetNumWrites() const { return num_writes_; }

private:
    std::string file_name_;
    std::fstream db_io_;
    
    // Explicit standard mutex
    std::mutex db_io_latch_;

    std::atomic<page_id_t> next_page_id_;
    std::atomic<int> num_flushes_{0};
    std::atomic<int> num_writes_{0};
};

} // namespace relational_db

#endif // DISK_MANAGER_H
