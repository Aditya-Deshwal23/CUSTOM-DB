#ifndef CLOCK_REPLACER_H
#define CLOCK_REPLACER_H

#include "config.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace relational_db {

class Replacer {
   public:
    virtual ~Replacer() = default;
    virtual bool Victim(frame_id_t* frame_id) = 0;
    virtual void Pin(frame_id_t frame_id) = 0;
    virtual void Unpin(frame_id_t frame_id) = 0;
    virtual size_t Size() const = 0;
    virtual bool ReferenceBit(frame_id_t frame_id) const = 0;
    virtual size_t ClockHand() const = 0;
};

class ClockReplacer final : public Replacer {
   public:
    explicit ClockReplacer(size_t num_pages);
    bool Victim(frame_id_t* frame_id) override;
    void Pin(frame_id_t frame_id) override;
    void Unpin(frame_id_t frame_id) override;
    size_t Size() const override;
    bool ReferenceBit(frame_id_t frame_id) const override;
    size_t ClockHand() const override;

   private:
    size_t capacity_;
    size_t clock_hand_{0};
    size_t evictable_count_{0};
    std::vector<bool> ref_bits_;
    std::vector<bool> evictable_;
    mutable std::mutex latch_;
};

class LruKReplacer final : public Replacer {
   public:
    explicit LruKReplacer(size_t num_pages, size_t k = 2);
    bool Victim(frame_id_t* frame_id) override;
    void Pin(frame_id_t frame_id) override;
    void Unpin(frame_id_t frame_id) override;
    size_t Size() const override;
    bool ReferenceBit(frame_id_t frame_id) const override;
    size_t ClockHand() const override;

   private:
    struct Candidate {
        bool mature{false};
        uint64_t oldest_kth_access{0};
        frame_id_t frame_id{-1};
        uint64_t generation{0};
    };

    struct CandidateLess {
        bool operator()(const Candidate& left, const Candidate& right) const {
            if (left.mature != right.mature) {
                return left.mature < right.mature;
            }
            if (left.oldest_kth_access != right.oldest_kth_access) {
                return left.oldest_kth_access < right.oldest_kth_access;
            }
            return left.frame_id < right.frame_id;
        }
    };

    struct CandidateHeapCompare {
        bool operator()(const Candidate& left, const Candidate& right) const {
            return CandidateLess{}(right, left);
        }
    };

    bool IsValid(frame_id_t frame_id) const;
    Candidate MakeCandidate(size_t index) const;
    void RebuildHeap();
    size_t capacity_;
    size_t k_;
    uint64_t timestamp_{0};
    size_t evictable_count_{0};
    std::vector<bool> evictable_;
    std::vector<std::deque<uint64_t>> histories_;
    std::vector<uint64_t> generations_;
    std::vector<Candidate> candidates_;
    mutable std::mutex latch_;
};

}  // namespace relational_db

#endif
