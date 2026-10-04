#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace loxis::v2 {

// Deep `--trace` logging.
//
// A null Trace* at any call site means "tracing off", so the compiler passes one
// down unconditionally and each hook is a single pointer test. Nothing is
// formatted unless tracing is actually on.
struct Trace {
    std::ostream* out = nullptr;

    bool enabled() const { return out != nullptr; }

    // A module entered the load order.
    void module(const std::string& path) const;

    // One pipeline stage finished; `ms` is wall-clock milliseconds.
    void stage(const std::string& name, double ms) const;

    // A declaration is about to be type-checked.
    void decl(const std::string& kind, const std::string& name) const;

    // A method/field lookup resolved. `candidates` is every name considered on
    // `recv`, which is what makes "field 'forEach' not found" actionable: you
    // can see what *was* available.
    void lookup(const std::string& what, const std::string& recv,
                const std::vector<std::string>& candidates, bool hit) const;
};

} // namespace loxis::v2
