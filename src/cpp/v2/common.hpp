#pragma once
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <string>
#include "Token.hpp"

namespace loxis::v2 {

struct CompileError {
    SourceLoc loc;
    std::string msg;
    std::string fmt() const { return loc.fmt() + ": error: " + msg; }
};

class ErrorReporter {
    std::vector<CompileError> errs;
public:
    void error(const SourceLoc& loc, const std::string& msg) {
        errs.push_back({loc, msg});
    }
    void error(const std::string& msg) {
        errs.push_back({{"<unknown>",0,0}, msg});
    }
    bool hasErrors() const { return !errs.empty(); }
    size_t count() const { return errs.size(); }
    void print() const {
        for (const auto& e : errs) std::cerr << e.fmt() << "\n";
    }
    const std::vector<CompileError>& errors() const { return errs; }
};

inline void panic(const std::string& msg) {
    std::cerr << "PANIC: " << msg << "\n";
    throw std::runtime_error(msg);
}

} // namespace
