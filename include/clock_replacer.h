#ifndef CLOCK_REPLACER_H
#define CLOCK_REPLACER_H

#include "config.h"
#include <vector>
#include <shared_mutex>

namespace relational_db {

/**
 * @brief ClockReplacer implements the Clock page replacement policy.
 */
class ClockReplacer {
public:
    explicit ClockReplacer(size_t num_pages);
    ~ClockReplacer() = default;

    bool Victim(frame_id_t *frame_id);
    void Pin(frame_id_t frame_id);
    void Unpin(frame_id_t frame_id);
    size_t Size();

private:
    size_t capacity_;
    size_t clock_hand_;
    std::vector<bool> ref_bits_;
    std::vector<bool> evictable_;
    
    // Explicit standard shared_mutex
    std::shared_mutex latch_;
};

} // namespace relational_db

#endif // CLOCK_REPLACER_H
