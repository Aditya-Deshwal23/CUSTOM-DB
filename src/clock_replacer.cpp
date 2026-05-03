#include "clock_replacer.h"
#include <mutex>
#include <shared_mutex>

namespace relational_db {

ClockReplacer::ClockReplacer(size_t num_pages)
    : capacity_(num_pages), clock_hand_(0), ref_bits_(num_pages, false), evictable_(num_pages, false) {}

bool ClockReplacer::Victim(frame_id_t *frame_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    
    // Check if any frame is evictable
    size_t evictable_count = 0;
    for (bool e : evictable_) if (e) ++evictable_count;
    if (evictable_count == 0) return false;

    size_t start_hand = clock_hand_;
    bool found = false;

    // Scan the clock for a victim
    do {
        if (evictable_[clock_hand_] && !ref_bits_[clock_hand_]) {
            *frame_id = static_cast<frame_id_t>(clock_hand_);
            evictable_[clock_hand_] = false; 
            found = true;
            break;
        } else if (ref_bits_[clock_hand_]) {
            ref_bits_[clock_hand_] = false;
        }
        clock_hand_ = (clock_hand_ + 1) % capacity_;
    } while (clock_hand_ != start_hand);

    if (!found) {
        do {
            if (evictable_[clock_hand_]) {
                *frame_id = static_cast<frame_id_t>(clock_hand_);
                evictable_[clock_hand_] = false;
                found = true;
                break;
            }
            clock_hand_ = (clock_hand_ + 1) % capacity_;
        } while (clock_hand_ != start_hand);
    }

    if (found) {
        clock_hand_ = (clock_hand_ + 1) % capacity_;
    }

    return found;
}

void ClockReplacer::Pin(frame_id_t frame_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    if (frame_id >= 0 && static_cast<size_t>(frame_id) < capacity_) {
        evictable_[frame_id] = false;
    }
}

void ClockReplacer::Unpin(frame_id_t frame_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    if (frame_id >= 0 && static_cast<size_t>(frame_id) < capacity_) {
        evictable_[frame_id] = true;
        ref_bits_[frame_id] = true; 
    }
}

size_t ClockReplacer::Size() {
    std::shared_lock<std::shared_mutex> lock(latch_);
    size_t count = 0;
    for (bool e : evictable_) {
        if (e) ++count;
    }
    return count;
}

} // namespace relational_db
