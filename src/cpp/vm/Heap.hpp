#pragma once

#include <cstdint>
#include <vector>
#include <utility>

// Heap allocator with type info for generic types.
// Header: [4B payload_size][4B type_id], then payload.
// All user-visible offsets point to the PAYLOAD start.

class Heap {
public:
	Heap() = default;

	// Allocate `size` bytes of type `typeId`, return payload offset
	uint32_t alloc(uint32_t size, uint32_t typeId);

	// Free allocation at `offset`
	void free(uint32_t offset);

	// Get type_id from an allocation header
	uint32_t getTypeId(uint32_t payloadOffset) const;

	// Store a 9-byte cell: 8 bytes int64 value + 1 byte tag
	void store(uint32_t offset, int64_t val, uint8_t tag);

	// Load a 9-byte cell
	std::pair <int64_t, uint8_t> load(uint32_t offset) const;

	std::vector <uint8_t> memory;
	static constexpr uint32_t
	SLOT_SIZE = 9;
};

inline uint32_t Heap::alloc(uint32_t size, uint32_t typeId) {
	uint32_t headerOffset = static_cast<uint32_t>(memory.size());
	// 4-byte size
	memory.push_back(static_cast<uint8_t>(size & 0xFF));
	memory.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
	memory.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
	memory.push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
	// 4-byte type_id
	memory.push_back(static_cast<uint8_t>(typeId & 0xFF));
	memory.push_back(static_cast<uint8_t>((typeId >> 8) & 0xFF));
	memory.push_back(static_cast<uint8_t>((typeId >> 16) & 0xFF));
	memory.push_back(static_cast<uint8_t>((typeId >> 24) & 0xFF));
	// Payload
	uint32_t payloadOffset = static_cast<uint32_t>(memory.size());
	memory.resize(memory.size() + size, 0);
	return payloadOffset;
}

inline void Heap::free(uint32_t offset) { (void) offset; }

inline uint32_t Heap::getTypeId(uint32_t payloadOffset) const {
	if (payloadOffset < 8) return 0;
	uint32_t r = 0;
	r |= static_cast<uint32_t>(memory[payloadOffset - 4]);
	r |= static_cast<uint32_t>(memory[payloadOffset - 3]) << 8;
	r |= static_cast<uint32_t>(memory[payloadOffset - 2]) << 16;
	r |= static_cast<uint32_t>(memory[payloadOffset - 1]) << 24;
	return r;
}

inline void Heap::store(uint32_t offset, int64_t val, uint8_t tag) {
	if (offset + 9 > memory.size()) return;
	uint64_t u = static_cast<uint64_t>(val);
	for (int i = 0; i < 8; i++)
		memory[offset + i] = static_cast<uint8_t>((u >> (i * 8)) & 0xFF);
	memory[offset + 8] = tag;
}

inline std::pair <int64_t, uint8_t> Heap::load(uint32_t offset) const {
	if (offset + 9 > memory.size()) return {0, 0};
	uint64_t u = 0;
	for (int i = 0; i < 8; i++)
		u |= (static_cast<uint64_t>(memory[offset + i]) << (i * 8));
	uint8_t tag = memory[offset + 8];
	return {static_cast<int64_t>(u), tag};
}
