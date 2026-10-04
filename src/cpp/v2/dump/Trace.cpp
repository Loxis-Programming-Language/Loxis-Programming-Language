#include "Trace.hpp"

#include <iomanip>
#include <ostream>

namespace loxis::v2 {

void Trace::module(const std::string& path) const {
    if (!enabled()) return;
    *out << "[module] " << path << "\n";
}

void Trace::stage(const std::string& name, double ms) const {
    if (!enabled()) return;
    *out << "[stage]  " << std::left << std::setw(16) << name << std::right
         << std::fixed << std::setprecision(1) << ms << " ms\n";
}

void Trace::decl(const std::string& kind, const std::string& name) const {
    if (!enabled()) return;
    *out << "[decl]   " << kind << " " << name << "\n";
}

void Trace::lookup(const std::string& what, const std::string& recv,
                   const std::vector<std::string>& candidates, bool hit) const {
    if (!enabled()) return;
    *out << "[lookup] " << what << " on " << recv
         << (hit ? " -> hit" : " -> MISSING") << "\n";
    if (candidates.empty()) {
        *out << "         candidates: (none)\n";
        return;
    }
    *out << "         candidates: " << candidates[0];
    for (size_t i = 1; i < candidates.size(); ++i) *out << ", " << candidates[i];
    *out << "\n";
}

} // namespace loxis::v2
