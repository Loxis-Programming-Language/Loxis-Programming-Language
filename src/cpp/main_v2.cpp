#include "v2/driver/Driver.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <file> [max_instructions] [verbose]\n";
            return 1;
        }

        std::string file = argv[1];
        int maxInstr = 1000000;
        bool verbose = false;

        if (argc >= 3) {
            std::string arg = argv[2];
            if (arg == "verbose" || arg == "true" || arg == "1" || arg == "yes") {
                verbose = true;
            } else {
                maxInstr = std::stoi(arg);
            }
        }
        if (argc >= 4) {
            std::string v = argv[3];
            if (v == "true" || v == "1" || v == "yes") {
                verbose = true;
            }
        }

        loxis::v2::Driver driver;
        return driver.compileAndRun(file, maxInstr, verbose);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
