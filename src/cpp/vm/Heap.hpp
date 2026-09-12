#pragma once

#include <cstdint>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

// Tag byte stored alongside every value, in registers, in globals and in heap
// cells. A cell tagged TAG_HEAP holds a Heap handle; everything else is a plain
// scalar or a string-pool index (strings are not heap objects and are never
// collected).
constexpr uint8_t TAG_INT = 0;
constexpr uint8_t TAG_STR = 1;
constexpr uint8_t TAG_HEAP = 2;
constexpr uint8_t TAG_FLOAT = 3;
constexpr uint8_t TAG_NULL = 4;

// Heap with stable object identity.
//
// A *handle* is an index into the slot table — it is NOT a byte offset. This
// matters because a GC backend must be able to invalidate an object (payload
// reused by a later allocation) while stale handles are still sitting in edge
// records and root sets; the slot's generation is what makes that detectable.
//
// Payloads live in `memory_` (one byte arena) and are addressed by byte offsets
// *within* the payload, so a field access is `loadCell(handle, fieldOffset)`.
// A payload is `cells` slots of 9 bytes (8B value + 1B tag), matching the VM's
// cell model — nothing is ever addressed across a payload boundary.
//
// Reclamation: freePayload() returns the payload to a size-class free list so a
// later allocation of the same cell count reuses those bytes. Only the GC
// backends call it (see GC.hpp); the language-level HFree opcode is a no-op by
// policy, because with a collector present the collector owns the lifetime.
class Heap {
public:
	static constexpr uint32_t INVALID = 0xFFFFFFFFu;
	static constexpr uint32_t SLOT_SIZE = 9;
	// Cells 1..SMALL_CLASSES-1 get a direct-indexed free list; larger payloads
	// go through a map so a huge array cannot inflate the table.
	static constexpr uint32_t SMALL_CLASSES = 65;

	struct ObjectSlot {
		uint32_t payloadOffset = INVALID;  // byte offset into memory_
		uint32_t cells = 0;                // allocated capacity in 9-byte cells
		uint32_t logicalSize = 0;          // bytes the caller asked for
		uint32_t typeId = 0;
		uint32_t generation = 0;           // bumped on every free
		bool live = false;
	};

	struct Stats {
		uint64_t allocations = 0;
		uint64_t reuses = 0;          // allocations served from a free list
		uint64_t frees = 0;
		uint64_t liveObjects = 0;
		uint64_t peakLiveObjects = 0;
		uint64_t bytesCommitted = 0;  // arena high-water mark
		uint64_t bytesLive = 0;
		uint64_t bytesReused = 0;
	};

	// Slot 0 is reserved and never handed out: the compiler lowers some null
	// tests to a numeric compare against 0, and byte offsets used to be non-zero
	// for the same reason. Handle 0 therefore always means "no object".
	Heap() { slots_.emplace_back(); }

	// Allocate `sizeBytes` of type `typeId`, return a handle. The payload is
	// zeroed, so reading an unwritten field yields the same thing it did when
	// the arena only ever grew.
	uint32_t alloc(uint32_t sizeBytes, uint32_t typeId);

	// Reclaim the payload and invalidate the handle. Callers are GC backends.
	void freePayload(uint32_t handle);

	// Fix the slot index space at `n` handles (AARC sizes its collector scratch
	// and ref table once, so the index space it accounts over must be bounded).
	// Allocation past the limit returns INVALID instead of growing.
	void reserveSlots(uint32_t n) {
		const uint32_t old = static_cast<uint32_t>(slots_.size());
		if (n > old) {
			slots_.resize(n);
			for (uint32_t i = old; i < n; i++) freeHandles_.push_back(i);
		}
		slotCap_ = n;
	}

	uint32_t slotCapacity() const { return static_cast<uint32_t>(slots_.size()); }

	bool valid(uint32_t handle) const {
		return handle < slots_.size() && slots_[handle].live;
	}

	const ObjectSlot& slot(uint32_t handle) const { return slots_[handle]; }

	uint32_t getTypeId(uint32_t handle) const {
		return handle < slots_.size() ? slots_[handle].typeId : 0;
	}

	uint32_t cellCount(uint32_t handle) const {
		return handle < slots_.size() ? slots_[handle].cells : 0;
	}

	// Store/load one 9-byte cell at `byteOff` inside the payload.
	void storeCell(uint32_t handle, uint32_t byteOff, int64_t val, uint8_t tag);
	std::pair<int64_t, uint8_t> loadCell(uint32_t handle, uint32_t byteOff) const;

	// Cell i of a payload, for collectors that must scan an object's fields.
	std::pair<int64_t, uint8_t> cellAt(uint32_t handle, uint32_t index) const {
		return loadCell(handle, index * SLOT_SIZE);
	}

	size_t slotCount() const { return slots_.size(); }
	size_t bytesCommitted() const { return memory_.size(); }
	const Stats& stats() const { return stats_; }

	void resetStats() { stats_ = Stats{}; }

private:
	std::vector<uint8_t> memory_;
	std::vector<ObjectSlot> slots_;
	std::vector<uint32_t> freeHandles_;
	uint32_t slotCap_ = 0;   // 0 = grow on demand
	std::array<std::vector<uint32_t>, SMALL_CLASSES> freeSmall_;
	std::unordered_map<uint32_t, std::vector<uint32_t>> freeLarge_;
	Stats stats_;
};

inline uint32_t Heap::alloc(uint32_t sizeBytes, uint32_t typeId) {
	uint32_t cells = (sizeBytes + SLOT_SIZE - 1) / SLOT_SIZE;
	if (cells == 0) cells = 1;

	uint32_t payloadOffset = INVALID;
	bool reused = false;

	if (cells < SMALL_CLASSES) {
		auto& list = freeSmall_[cells];
		if (!list.empty()) {
			payloadOffset = list.back();
			list.pop_back();
			reused = true;
		}
	} else {
		auto it = freeLarge_.find(cells);
		if (it != freeLarge_.end() && !it->second.empty()) {
			payloadOffset = it->second.back();
			it->second.pop_back();
			reused = true;
		}
	}

	const size_t payloadBytes = static_cast<size_t>(cells) * SLOT_SIZE;
	if (!reused) {
		payloadOffset = static_cast<uint32_t>(memory_.size());
		memory_.resize(memory_.size() + payloadBytes, 0);
	} else {
		std::fill(memory_.begin() + payloadOffset,
		          memory_.begin() + payloadOffset + static_cast<ptrdiff_t>(payloadBytes),
		          static_cast<uint8_t>(0));
		stats_.reuses++;
		stats_.bytesReused += payloadBytes;
	}

	uint32_t handle;
	if (!freeHandles_.empty()) {
		handle = freeHandles_.back();
		freeHandles_.pop_back();
	} else {
		if (slotCap_ != 0 && slots_.size() >= slotCap_) return INVALID;
		handle = static_cast<uint32_t>(slots_.size());
		slots_.emplace_back();
	}

	ObjectSlot& s = slots_[handle];
	s.payloadOffset = payloadOffset;
	s.cells = cells;
	s.logicalSize = sizeBytes;
	s.typeId = typeId;
	s.live = true;

	stats_.allocations++;
	stats_.liveObjects++;
	stats_.bytesLive += payloadBytes;
	if (stats_.liveObjects > stats_.peakLiveObjects) {
		stats_.peakLiveObjects = stats_.liveObjects;
	}
	stats_.bytesCommitted = memory_.size();
	return handle;
}

inline void Heap::freePayload(uint32_t handle) {
	if (!valid(handle)) return;
	ObjectSlot& s = slots_[handle];

	const size_t payloadBytes = static_cast<size_t>(s.cells) * SLOT_SIZE;
	if (s.cells < SMALL_CLASSES) {
		freeSmall_[s.cells].push_back(s.payloadOffset);
	} else {
		freeLarge_[s.cells].push_back(s.payloadOffset);
	}

	stats_.bytesLive -= payloadBytes;
	s.live = false;
	s.payloadOffset = INVALID;
	s.cells = 0;
	s.logicalSize = 0;
	s.typeId = 0;
	s.generation++;
	freeHandles_.push_back(handle);

	stats_.frees++;
	stats_.liveObjects--;
}

inline void Heap::storeCell(uint32_t handle, uint32_t byteOff, int64_t val, uint8_t tag) {
	if (handle >= slots_.size()) return;
	const ObjectSlot& s = slots_[handle];
	if (!s.live || s.payloadOffset == INVALID) return;
	// Bounds are the caller's request, not the allocator's rounding: a padded
	// payload must not silently legalize a store past what was asked for.
	if (byteOff + SLOT_SIZE > s.logicalSize) return;

	uint32_t offset = s.payloadOffset + byteOff;
	uint64_t u = static_cast<uint64_t>(val);
	for (int i = 0; i < 8; i++)
		memory_[offset + i] = static_cast<uint8_t>((u >> (i * 8)) & 0xFF);
	memory_[offset + 8] = tag;
}

inline std::pair<int64_t, uint8_t> Heap::loadCell(uint32_t handle, uint32_t byteOff) const {
	if (handle >= slots_.size()) return {0, 0};
	const ObjectSlot& s = slots_[handle];
	if (!s.live || s.payloadOffset == INVALID) return {0, 0};
	if (byteOff + SLOT_SIZE > s.logicalSize) return {0, 0};

	uint32_t offset = s.payloadOffset + byteOff;
	uint64_t u = 0;
	for (int i = 0; i < 8; i++)
		u |= (static_cast<uint64_t>(memory_[offset + i]) << (i * 8));
	return {static_cast<int64_t>(u), memory_[offset + 8]};
}
