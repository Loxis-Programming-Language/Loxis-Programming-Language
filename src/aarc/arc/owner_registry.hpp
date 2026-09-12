// AARC adaptive owner lifetime registry.

#pragma once

#include "../core/cell.hpp"
#include "../core/owner.hpp"

#include <array>
#include <mutex>

namespace loxis { namespace aarc {

enum class OwnerState : uint8_t { Free, Active, Closing };

class OwnerRegistry {
public:
    bool acquire(OwnerToken& out) {
        std::lock_guard<std::mutex> lk(mu_);
        for (uint16_t id = 0; id < CellPool::kMaxOwners; ++id) {
            Entry& e = entries_[id];
            if (e.state != OwnerState::Free) continue;
            e.state = OwnerState::Active;
            out = OwnerToken{id, e.generation};
            return true;
        }
        return false;
    }

    bool beginClose(OwnerToken token) {
        std::lock_guard<std::mutex> lk(mu_);
        Entry* e = current(token);
        if (!e || e->state != OwnerState::Active) return false;
        e->state = OwnerState::Closing;
        return true;
    }

    bool release(OwnerToken token) {
        std::lock_guard<std::mutex> lk(mu_);
        Entry* e = current(token);
        if (!e || e->state != OwnerState::Closing) return false;
        ++e->generation;
        if (e->generation == 0) ++e->generation;
        e->state = OwnerState::Free;
        return true;
    }

    OwnerState state(OwnerToken token) const {
        std::lock_guard<std::mutex> lk(mu_);
        const Entry* e = current(token);
        return e ? e->state : OwnerState::Free;
    }

private:
    struct Entry {
        OwnerState state = OwnerState::Free;
        uint32_t generation = 1;
    };

    Entry* current(OwnerToken token) {
        if (token.id >= entries_.size()) return nullptr;
        Entry& e = entries_[token.id];
        return e.generation == token.generation ? &e : nullptr;
    }
    const Entry* current(OwnerToken token) const {
        if (token.id >= entries_.size()) return nullptr;
        const Entry& e = entries_[token.id];
        return e.generation == token.generation ? &e : nullptr;
    }

    mutable std::mutex mu_;
    std::array<Entry, CellPool::kMaxOwners> entries_{};
};

}} // namespace loxis::aarc
