#pragma once

#include "Heap.hpp"
#include "arc/engine.hpp"

// The narrow "external object source" AARC accounts over.
//
// AARC keeps its own metadata — refcounts, root counts, the edge graph, suspect
// marks, Tarjan scratch — and never touches payload bytes. The Loxis Heap keeps
// object storage — payloads, sizes, free lists, actual byte reuse. This class is
// the entire coupling between them: liveness, generation, and "release this
// object's storage".
//
// The generation is the ABA guard: AARC's RefTable rows and queued messages
// carry ObjHandle{index, gen}, and a slot whose payload was freed and reused
// never matches a handle minted before the free.
class AarcCellSource {
public:
	explicit AarcCellSource(Heap& heap) : heap_(heap) {}

	uint32_t capacity() const { return heap_.slotCapacity(); }

	bool valid(loxis::aarc::ObjHandle h) const {
		return h.index < heap_.slotCapacity() && heap_.valid(h.index) &&
		       heap_.slot(h.index).generation == h.gen;
	}

	bool isLiveAt(uint32_t index) const { return heap_.valid(index); }

	uint32_t generationOf(uint32_t index) const {
		return index < heap_.slotCapacity() ? heap_.slot(index).generation : 0;
	}

	// The collector decided the object is unreachable: hand the payload back to
	// the allocator and invalidate every handle that pointed at this slot.
	bool free(loxis::aarc::ObjHandle h) {
		if (!valid(h)) {
			++errs_;
			return false;
		}
		heap_.freePayload(h.index);
		return true;
	}

	// The Loxis VM runs exactly one LOCAL engine over the whole heap, so every
	// slot belongs to that regime/owner. Providing the regime API here keeps
	// the engine's authority checks usable without teaching AARC about Loxis.
	loxis::aarc::Regime regimeOf(loxis::aarc::ObjHandle) const {
		return loxis::aarc::Regime::Local;
	}
	loxis::aarc::Regime regimeAt(uint32_t) const {
		return loxis::aarc::Regime::Local;
	}
	loxis::aarc::OwnerToken ownerTokenAt(uint32_t) const { return owner_; }

	template <class F>
	void forEachLive(F&& cb) const {
		for (uint32_t i = 1; i < heap_.slotCapacity(); ++i) {
			if (heap_.valid(i)) cb(loxis::aarc::ObjHandle{i, heap_.slot(i).generation});
		}
	}

	uint64_t errorCount() const { return errs_; }

	static constexpr loxis::aarc::OwnerToken kOwner{0, 1};

private:
	Heap& heap_;
	loxis::aarc::OwnerToken owner_ = kOwner;
	uint64_t errs_ = 0;
};
