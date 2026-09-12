#include "v2/driver/Driver.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0]
                      << " <file> [max_instructions] [verbose] [--gc=nogc|arc|aarc]"
                         " [--slots=N] [--gc-stats]\n";
            return 1;
        }

        loxis::v2::RunOptions opts;
        opts.file = argv[1];

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg.rfind("--gc=", 0) == 0) {
                if (!parseGCMode(arg.substr(5), opts.gc)) {
                    std::cerr << "Unknown GC backend: " << arg.substr(5)
                              << " (expected nogc, arc or aarc)\n";
                    return 1;
                }
            } else if (arg.rfind("--slots=", 0) == 0) {
                opts.heapSlots = static_cast<uint32_t>(std::stoul(arg.substr(8)));
            } else if (arg == "--gc-stats") {
                opts.gcStats = true;
            } else if (arg == "verbose" || arg == "true" || arg == "yes") {
                opts.verbose = true;
            } else if (arg == "1") {
                opts.verbose = true;
            } else {
                opts.maxInstr = std::stoi(arg);
            }
        }

        loxis::v2::Driver driver;
        return driver.compileAndRun(opts);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
