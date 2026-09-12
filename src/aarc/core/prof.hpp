// Optional cycle-level producer-path profiling (AARC_PROFILE builds only).
//
// The producer hot path is ~180ns/pair, and one rdtsc pair costs ~10ns, so
// timing every op would dwarf what we are attributing.  Sample every 64th op
// instead: the op mix in the tight benchmark loop is uniform, so the sample is
// unbiased and the instrumentation stays ~1/64 of a timing pair.
//
// Everything here compiles to nothing when AARC_PROFILE is off (the default).

#pragma once

#if AARC_PROFILE
#include <x86intrin.h>
#include <cstdint>

namespace loxis { namespace aarc {

constexpr uint64_t kProfMask = 63;

inline bool profSample(uint64_t& opCounter) {
    return (++opCounter & kProfMask) == 0;
}

// Serialized TSC read.  A bare __rdtsc() is NOT ordered against the surrounding
// code -- the CPU may read the counter before earlier work completes, which
// makes short intervals read far too small.  lfence on both sides pins it.
inline uint64_t rdtscSerial() {
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// Times a scope when enabled; returns 0 otherwise (no rdtsc issued).
struct ProfTsc {
    uint64_t t0 = 0;
    bool on = false;
    explicit ProfTsc(bool enable) : on(enable) {
        if (on) t0 = rdtscSerial();
    }
    uint64_t stop() { return on ? rdtscSerial() - t0 : 0; }
};

}} // namespace loxis::aarc
#endif
