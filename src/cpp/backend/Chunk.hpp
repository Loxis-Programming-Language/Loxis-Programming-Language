#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <stdexcept>
#include "../ir/OpCode.hpp"
#include "../ir/Type.hpp"

enum class OperandTag : uint8_t {
	Reg = 0x01,  // 1-byte register ID
	Imm64 = 0x02,  // 8-byte signed int
	StrIdx = 0x03,  // 4-byte string pool index
	Addr = 0x04   // 4-byte bytecode address
};

struct Chunk {
	std::vector <uint8_t> code;
	std::vector <std::string> strings;  // Interned string pool
	std::unordered_map <std::string, size_t> stringIndices;

	// Type pool — preserves generic types in bytecode (no erasure)
	std::vector <std::vector<uint8_t>> typePool;
	std::unordered_map <std::string, uint32_t> typeIndices;

	Chunk() = default;

	uint32_t internType(const TypeNode &type) {
		std::vector <uint8_t> buf;
		type.serialize(buf);
		std::string key(reinterpret_cast<const char *>(buf.data()), buf.size());
		auto it = typeIndices.find(key);
		if (it != typeIndices.end()) return it->second;
		uint32_t idx = static_cast<uint32_t>(typePool.size());
		typePool.push_back(std::move(buf));
		typeIndices[key] = idx;
		return idx;
	}

	uint32_t internString(const std::string &str) {
		auto it = stringIndices.find(str);
		if (it != stringIndices.end()) {
			return static_cast<uint32_t>(it->second);
		}

		uint32_t index = static_cast<uint32_t>(strings.size());
		strings.push_back(str);
		stringIndices[str] = index;
		return index;
	}

	void emitByte(uint8_t byte) {
		code.push_back(byte);
	}

	void emitU32(uint32_t value) {
		code.push_back(static_cast<uint8_t>((value >> 0) & 0xFF));
		code.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
		code.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
		code.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
	}

	void emitI64(int64_t value) {
		uint64_t u = static_cast<uint64_t>(value);
		code.push_back(static_cast<uint8_t>((u >> 0) & 0xFF));
		code.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
		code.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
		code.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
		code.push_back(static_cast<uint8_t>((u >> 32) & 0xFF));
		code.push_back(static_cast<uint8_t>((u >> 40) & 0xFF));
		code.push_back(static_cast<uint8_t>((u >> 48) & 0xFF));
		code.push_back(static_cast<uint8_t>((u >> 56) & 0xFF));
	}

	void patchU32(size_t offset, uint32_t value) {
		if (offset + 3 >= code.size()) {
			throw std::runtime_error("patch offset out of bounds");
		}
		code[offset] = static_cast<uint8_t>((value >> 0) & 0xFF);
		code[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
		code[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
		code[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
	}

	size_t offset() const {
		return code.size();
	}
};
