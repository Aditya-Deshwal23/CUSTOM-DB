#include "clock_replacer.h"

#include <algorithm>
#include <limits>

namespace relational_db {

ClockReplacer::ClockReplacer(size_t num_pages)
    : capacity_(num_pages), ref_bits_(num_pages, false), evictable_(num_pages, false) {}

bool ClockReplacer::Victim(frame_id_t* frame_id) {
    std::lock_guard<std::mutex> lock(latch_);
    if (frame_id == nullptr || evictable_count_ == 0 || capacity_ == 0) {
        return false;
    }

    const size_t scan_limit = capacity_ * 2U;
    for (size_t scanned = 0; scanned < scan_limit; ++scanned) {
        const size_t current = clock_hand_;
        clock_hand_ = (clock_hand_ + 1U) % capacity_;
        if (!evictable_[current]) {
            continue;
        }
        if (ref_bits_[current]) {
            ref_bits_[current] = false;
            continue;
        }
        evictable_[current] = false;
        --evictable_count_;
        *frame_id = static_cast<frame_id_t>(current);
        return true;
    }

    for (size_t scanned = 0; scanned < capacity_; ++scanned) {
        const size_t current = clock_hand_;
        clock_hand_ = (clock_hand_ + 1U) % capacity_;
        if (evictable_[current]) {
            evictable_[current] = false;
            ref_bits_[current] = false;
            --evictable_count_;
            *frame_id = static_cast<frame_id_t>(current);
            return true;
        }
    }
    return false;
}

void ClockReplacer::Pin(frame_id_t frame_id) {
    std::lock_guard<std::mutex> lock(latch_);
    if (frame_id >= 0 && static_cast<size_t>(frame_id) < capacity_ &&
        evictable_[static_cast<size_t>(frame_id)]) {
        evictable_[static_cast<size_t>(frame_id)] = false;
        --evictable_count_;
    }
}

void ClockReplacer::Unpin(frame_id_t frame_id) {
    std::lock_guard<std::mutex> lock(latch_);
    if (frame_id >= 0 && static_cast<size_t>(frame_id) < capacity_) {
        const size_t index = static_cast<size_t>(frame_id);
        if (!evictable_[index]) {
            evictable_[index] = true;
            ++evictable_count_;
        }
        ref_bits_[index] = true;
    }
}

size_t ClockReplacer::Size() const {
    std::lock_guard<std::mutex> lock(latch_);
    return evictable_count_;
}

bool ClockReplacer::ReferenceBit(frame_id_t frame_id) const {
    std::lock_guard<std::mutex> lock(latch_);
    return frame_id >= 0 && static_cast<size_t>(frame_id) < capacity_ &&
           ref_bits_[static_cast<size_t>(frame_id)];
}

size_t ClockReplacer::ClockHand() const {
    std::lock_guard<std::mutex> lock(latch_);
    return clock_hand_;
}

LruKReplacer::LruKReplacer(size_t num_pages, size_t k)
    : capacity_(num_pages), k_(k == 0 ? 1 : k), evictable_(num_pages, false),
      histories_(num_pages), generations_(num_pages, 0) {
    candidates_.reserve(num_pages * 4U + 1U);
}

bool LruKReplacer::IsValid(frame_id_t frame_id) const {
    return frame_id >= 0 && static_cast<size_t>(frame_id) < capacity_;
}

LruKReplacer::Candidate LruKReplacer::MakeCandidate(size_t index) const {
    const auto& history = histories_[index];
    return Candidate{history.size() >= k_, history.empty() ? 0 : history.front(),
                     static_cast<frame_id_t>(index), generations_[index]};
}

void LruKReplacer::RebuildHeap() {
    candidates_.clear();
    for (size_t index = 0; index < capacity_; ++index) {
        if (evictable_[index]) {
            candidates_.push_back(MakeCandidate(index));
        }
    }
    std::make_heap(candidates_.begin(), candidates_.end(), CandidateHeapCompare{});
}

bool LruKReplacer::Victim(frame_id_t* frame_id) {
    std::lock_guard<std::mutex> lock(latch_);
    if (frame_id == nullptr || evictable_count_ == 0) {
        return false;
    }

    while (!candidates_.empty()) {
        std::pop_heap(candidates_.begin(), candidates_.end(), CandidateHeapCompare{});
        const Candidate candidate = candidates_.back();
        candidates_.pop_back();
        const size_t victim = static_cast<size_t>(candidate.frame_id);
        if (!evictable_[victim] || generations_[victim] != candidate.generation) {
            continue;
        }
        evictable_[victim] = false;
        --evictable_count_;
        ++generations_[victim];
        *frame_id = candidate.frame_id;
        histories_[victim].clear();
        return true;
    }
    return false;
}

void LruKReplacer::Pin(frame_id_t frame_id) {
    std::lock_guard<std::mutex> lock(latch_);
    if (IsValid(frame_id) && evictable_[static_cast<size_t>(frame_id)]) {
        const size_t index = static_cast<size_t>(frame_id);
        evictable_[index] = false;
        --evictable_count_;
        ++generations_[index];
    }
}

void LruKReplacer::Unpin(frame_id_t frame_id) {
    std::lock_guard<std::mutex> lock(latch_);
    if (!IsValid(frame_id)) {
        return;
    }
    const size_t index = static_cast<size_t>(frame_id);
    if (!evictable_[index]) {
        evictable_[index] = true;
        ++evictable_count_;
    }
    if (timestamp_ != std::numeric_limits<uint64_t>::max()) {
        ++timestamp_;
    }
    histories_[index].push_back(timestamp_);
    if (histories_[index].size() > k_) {
        histories_[index].pop_front();
    }
    ++generations_[index];
    candidates_.push_back(MakeCandidate(index));
    std::push_heap(candidates_.begin(), candidates_.end(), CandidateHeapCompare{});
    if (candidates_.size() > capacity_ * 4U) {
        RebuildHeap();
    }
}

size_t LruKReplacer::Size() const {
    std::lock_guard<std::mutex> lock(latch_);
    return evictable_count_;
}

bool LruKReplacer::ReferenceBit(frame_id_t frame_id) const {
    std::lock_guard<std::mutex> lock(latch_);
    return IsValid(frame_id) && !histories_[static_cast<size_t>(frame_id)].empty();
}

size_t LruKReplacer::ClockHand() const {
    return 0;
}

}  // namespace relational_db
