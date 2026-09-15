#pragma once
#include <string>
#include <vector>
#include "../../backend/Chunk.hpp"
#include "../../vm/GC.hpp"

namespace loxis::v2 {

struct RunOptions {
    std::string file;
    std::vector<std::string> arguments;
    int maxInstr = 1000000;
    bool verbose = false;
    GCMode gc = GCMode::NoGC;
    bool gcStats = false;
    uint32_t heapSlots = 0;   // 0 = backend default (AARC fixes the slot space)
};

class Driver {
public:
    // Full pipeline up to bytecode, no execution — the benchmark harness needs
    // to run one compiled Chunk under several GC backends.
    int compileToChunk(const std::string& file, bool verbose, Chunk& out);

    int compileAndRun(const RunOptions& opts);
};

} // namespace loxis::v2
