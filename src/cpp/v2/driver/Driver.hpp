#pragma once
#include <string>

namespace loxis::v2 {

class Driver {
public:
    int compileAndRun(const std::string& file, int maxInstr, bool verbose);
};

} // namespace loxis::v2
