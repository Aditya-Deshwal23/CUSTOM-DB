#include "disk_manager.h"
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <mutex>

namespace relational_db {

DiskManager::DiskManager(const std::string& db_file) : file_name_(db_file), next_page_id_(0) {
    db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
    
    if (!db_io_.is_open()) {
        db_io_.clear();
        db_io_.open(db_file, std::ios::binary | std::ios::trunc | std::ios::out);
        db_io_.close();
        
        db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
        if (!db_io_.is_open()) {
            throw std::runtime_error("Can't open db file");
        }
    } else {
        db_io_.seekg(0, std::ios::end);
        size_t file_size = db_io_.tellg();
        next_page_id_ = static_cast<page_id_t>(file_size / PAGE_SIZE);
    }
}

DiskManager::~DiskManager() {
    db_io_.close();
}

void DiskManager::WritePage(page_id_t page_id, const char* page_data) {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    
    size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;
    db_io_.seekp(offset);
    db_io_.write(page_data, PAGE_SIZE);
    
    if (db_io_.bad()) {
        throw std::runtime_error("I/O error while writing");
    }
    
    db_io_.flush(); 
    num_writes_++;
    num_flushes_++;
}

void DiskManager::ReadPage(page_id_t page_id, char* page_data) {
    std::lock_guard<std::mutex> lock(db_io_latch_);
    
    size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;
    
    db_io_.seekg(0, std::ios::end);
    size_t file_size = db_io_.tellg();
    if (offset >= file_size) {
        std::memset(page_data, 0, PAGE_SIZE);
        return;
    }
    
    db_io_.seekg(offset);
    db_io_.read(page_data, PAGE_SIZE);
    
    if (db_io_.bad()) {
        throw std::runtime_error("I/O error while reading");
    }
    
    int read_count = db_io_.gcount();
    if (read_count < PAGE_SIZE) {
        std::memset(page_data + read_count, 0, PAGE_SIZE - read_count);
    }
}

page_id_t DiskManager::AllocatePage() {
    return next_page_id_++;
}

} // namespace relational_db
