#pragma once
#include <string>
#include <vector>
#include "../../backend/Chunk.hpp"
#include "../../vm/GC.hpp"

namespace loxis::v2 {

// Which IR stages to print. Any flag set makes the driver compile-only: the
// dump goes to stdout and the VM is never constructed, so IR text and program
// output can never interleave.
struct DumpOptions {
    bool tokens = false;
    bool ast = false;
    bool hir = false;
    bool mir = false;
    bool bytecode = false;
    bool scopes = false;

    bool any() const { return tokens || ast || hir || mir || bytecode || scopes; }
};

struct RunOptions {
    std::string file;
    std::string stdlibRoot;
    std::vector<std::string> arguments;
    int maxInstr = 1000000;
    bool verbose = false;
    bool trace = false;
    bool compileOnly = false;
    DumpOptions dumps;
    GCMode gc = GCMode::NoGC;
    bool gcStats = false;
    uint32_t heapSlots = 0;   // 0 = backend default (AARC fixes the slot space)
};

class Driver {
public:
    // Full pipeline up to bytecode, no execution — the benchmark harness needs
    // to run one compiled Chunk under several GC backends.
    int compileToChunk(const std::string& file, bool verbose, Chunk& out);

    // Same pipeline, but honours verbose/trace/dump options.
    int compileToChunk(const RunOptions& opts, Chunk& out);

    int compileAndRun(const RunOptions& opts);
};

} // namespace loxis::v2
