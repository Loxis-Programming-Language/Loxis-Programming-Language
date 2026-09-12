// AARC owner identity: a reusable slot id plus a generation.
//
// Slot generations prevent a delayed promotion request or a live SHARED cell
// from being mistaken for state owned by a later user of the same owner id.

#pragma once

#include <cstdint>

namespace loxis { namespace aarc {

struct OwnerToken {
    uint16_t id = 0xFFFFu;
    uint32_t generation = 0;
};

constexpr OwnerToken kNoOwnerToken{0xFFFFu, 0u};

inline bool operator==(OwnerToken a, OwnerToken b) {
    return a.id == b.id && a.generation == b.generation;
}
inline bool operator!=(OwnerToken a, OwnerToken b) { return !(a == b); }

}} // namespace loxis::aarc
