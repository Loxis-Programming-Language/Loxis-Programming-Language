#pragma once

#include <stdexcept>
#include <string>
#include <sstream>
#include <cstdint>

struct SourceLocation {
	std::string file;
	uint32_t line = 0;
	uint32_t col = 0;

	std::string format() const {
		std::ostringstream oss;
		oss << file << ":" << line << ":" << col;
		return oss.str();
	}
};

struct GalVMError : std::runtime_error {
	SourceLocation loc;

	GalVMError(const std::string &msg, const SourceLocation &location)
			: std::runtime_error(msg), loc(location) {}

	GalVMError(const std::string &msg)
			: std::runtime_error(msg), loc{"<unknown>", 0, 0} {}
};

struct LexError : GalVMError {
	using GalVMError::GalVMError;
};

struct ParseError : GalVMError {
	using GalVMError::GalVMError;
};

struct IRError : GalVMError {
	using GalVMError::GalVMError;
};

struct RuntimeError : GalVMError {
	using GalVMError::GalVMError;
};
