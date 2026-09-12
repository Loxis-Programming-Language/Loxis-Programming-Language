#pragma once

#include "Heap.hpp"
#include "AarcCellSource.hpp"
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

// Lifecycle bookkeeping backends for the VM.
//
// The VM owns exactly one backend and funnels *every* mutation through the same
// barrier shape, regardless of backend:
//
//   register/global overwrite -> rootRemove(old), rootAdd(new)
//   heap cell overwrite       -> edgeRemove(owner, oldCell), edgeAdd(owner, newCell)
//
// That is the point of the experiment: the allocator, the object layout, the
// dispatch loop and the barrier insertion are identical, so a difference
// between two backends is a difference of *algorithm*, not of scaffolding.
// The NoGC backend implements all of the above as no-ops, so it pays the same
// call overhead and is a lower bound rather than an idealized column.

enum class GCMode {
	NoGC,
	ARC,
	AARCLocal,
};

// Same counter set for every backend so the harness can print one table.
struct GCMetrics {
	uint64_t allocations = 0;
	uint64_t reclaimedObjects = 0;
	uint64_t rootAdds = 0;
	uint64_t rootRemoves = 0;
	uint64_t edgeAdds = 0;
	uint64_t edgeRemoves = 0;
	uint64_t cycleCollections = 0;
	uint64_t cycleObjectsReclaimed = 0;
	// Cycle-scan work: how many scans ran and how much of the graph they
	// touched. A scan that visits a lot and reclaims nothing is the signal the
	// adaptive trigger is built on.
	uint64_t cycleScans = 0;
	uint64_t cycleScanVisited = 0;
	uint64_t cycleReclaimedBytes = 0;   // payload bytes handed back by cycle passes
	uint64_t cycleCollectNs = 0;        // wall time spent inside cycle collection
	uint64_t cycleNsMax = 0;            // worst single cycle-collection pause
	uint64_t cycleObjectsByPolicy = 0;  // reclaimed by a policy trigger, not by teardown
	uint64_t suspectBacklog = 0;   // pending suspects at the end of the run

	// Why the adaptive trigger fired (sums to cycleCollections).
	uint64_t triggerSuspectPressure = 0;
	uint64_t triggerHeapPressure = 0;
	uint64_t triggerStarvation = 0;
	uint64_t triggerFixedInterval = 0;

	// Adaptive threshold trajectory.
	uint64_t thresholdRaises = 0;
	uint64_t thresholdLowers = 0;
	uint32_t thresholdFinal = 0;
	uint32_t thresholdMin = 0;
	uint32_t thresholdMax = 0;
	uint64_t explicitFreesIgnored = 0;
	uint64_t invalidHandleOps = 0;
};

class GCBackend {
public:
	virtual ~GCBackend() = default;

	virtual const char* name() const = 0;
	virtual void attach(Heap* heap) { m_heap = heap; }

	// A handle was handed to the mutator. The slot is already zeroed.
	virtual void onAlloc(uint32_t handle) { (void) handle; }

	virtual void rootAdd(uint32_t handle) { (void) handle; }
	virtual void rootRemove(uint32_t handle) { (void) handle; }
	virtual void edgeAdd(uint32_t from, uint32_t to) { (void) from; (void) to; }
	virtual void edgeRemove(uint32_t from, uint32_t to) { (void) from; (void) to; }

	// Policy: the language-level HFree opcode is a no-op under every backend.
	// With a collector present the collector owns the lifetime; a real free
	// here would let a live reference dangle.
	virtual void onExplicitFree(uint32_t handle) { (void) handle; }

	// Everything still reachable at exit is a leak; backends may report it.
	virtual void shutdown() {}

	// Collector-specific tuning hook; backends that have nothing to tune
	// ignore it. Spec grammar lives with the backend ("adaptive", "fixed:N").
	virtual void setCyclePolicy(const std::string& spec) { (void) spec; }

	const GCMetrics& metrics() const { return m_metrics; }
	virtual void dumpMetrics(std::ostream& os) const;

protected:
	Heap* m_heap = nullptr;
	GCMetrics m_metrics;
};

// Baseline: allocate and never reclaim. The heap grows without bound; the
// payload bytes are still managed by Heap so allocation cost stays comparable.
class NoGCBackend : public GCBackend {
public:
	const char* name() const override { return "nogc"; }
	void onAlloc(uint32_t handle) override;
	void rootAdd(uint32_t handle) override;
	void rootRemove(uint32_t handle) override;
	void edgeAdd(uint32_t from, uint32_t to) override;
	void edgeRemove(uint32_t from, uint32_t to) override;
	void onExplicitFree(uint32_t handle) override;
	void shutdown() override;
};

// Classic reference counting. Reclamation is a worklist walk over the payload's
// 9-byte cells, so a dead object's children are released exactly once each.
// Self-cycles leak by construction — that is the thing AARC is supposed to fix.
class ArcBackend : public GCBackend {
public:
	const char* name() const override { return "arc"; }

	void attach(Heap* heap) override;
	void onAlloc(uint32_t handle) override;
	void rootAdd(uint32_t handle) override;
	void rootRemove(uint32_t handle) override;
	void edgeAdd(uint32_t from, uint32_t to) override;
	void edgeRemove(uint32_t from, uint32_t to) override;
	void onExplicitFree(uint32_t handle) override;
	void shutdown() override;

private:
	void retain(uint32_t handle);
	void release(uint32_t handle);
	void reclaim(uint32_t handle);

	std::vector<uint32_t> m_rc;    // refcount per handle slot
	std::vector<uint32_t> m_work;  // reclaim worklist
};

// AARC in its LOCAL (inline, single-owner) regime: the full cycle-safe engine
// runs synchronously on the mutator thread.  It owns refcounts, roots, the edge
// graph and cycle detection; the Loxis Heap owns payload storage.  Every op is
// applied immediately (no queue, no worker), and a drain-point collection runs
// every kSweepCadence ops plus once at shutdown.
class LocalAarcBackend : public GCBackend {
public:
	// Below this many visited nodes a scan is too small to justify retuning.
	static constexpr uint64_t kYieldMinVisited = 256;

	explicit LocalAarcBackend(Heap& heap);

	const char* name() const override { return "aarc-local"; }

	void onAlloc(uint32_t handle) override;
	void rootAdd(uint32_t handle) override;
	void rootRemove(uint32_t handle) override;
	void edgeAdd(uint32_t from, uint32_t to) override;
	void edgeRemove(uint32_t from, uint32_t to) override;
	void onExplicitFree(uint32_t handle) override;
	void shutdown() override;

	uint64_t collectorErrors() const { return m_engine.totalErrors() + m_cells.errorCount(); }
	void collect() { m_engine.sweep(); }
	void dumpMetrics(std::ostream& os) const override;
	void setCyclePolicy(const std::string& spec) override;

	size_t pendingSuspects() const { return m_engine.pendingSuspects(); }
	uint32_t suspectThreshold() const { return m_suspectThreshold; }
	bool adaptive() const { return m_adaptive; }

	// Per-scan samples, index-aligned: what one Tarjan pass cost and how much of
	// the graph it touched. Kept so the harness can report a pause distribution
	// rather than only a total — "no global STW" still has to answer "how long
	// can one local collection block the mutator".
	const std::vector<uint32_t>& scanNs() const { return m_scanNs; }
	const std::vector<uint32_t>& scanVisited() const { return m_scanVisited; }
	const std::vector<uint32_t>& scanTriggerSuspects() const { return m_scanTriggerSuspects; }
	const std::vector<uint32_t>& scanLiveSuspects() const { return m_scanLiveSuspects; }

	// One entry per scan, first 48 only: what the trigger saw and what the pass
	// actually did. Used to read a policy's trajectory rather than its totals.
	struct ScanSample {
		uint64_t ops;
		uint32_t trigger;
		uint32_t live;
		uint32_t visited;
		uint32_t reclaimed;
		uint32_t threshold;
		uint32_t stackAtTrigger;  // raw lazy-stack entries (the old signal)
		uint32_t engRoots;      // last pass's roots (engine-side)
		uint32_t engVisited;    // last pass's reachable set
		uint32_t skipDisc;      // roots already discovered in that pass
		uint32_t skipDead;      // roots not live
	};
	const std::vector<ScanSample>& scanTrace() const { return m_scanTrace; }
	const std::vector<std::pair<uint64_t, uint32_t>>& thresholdTrace() const {
		return m_thresholdTrace;
	}

	// Cycle-trigger policy.  Zero-chain reclamation stays eager (it is cheap
	// and it is what keeps the heap small); only the Tarjan scan is gated,
	// because a scan that finds nothing is pure waste and a scan that runs too
	// rarely lets cyclic garbage pile up.
	struct CyclePolicy {
		bool adaptive = true;
		uint32_t fixedInterval = 1024;     // fixed mode: scan every N barrier ops
		uint32_t suspectThreshold = 4096;  // adaptive: scan once this many pile up
		uint32_t suspectThresholdMin = 512;
		uint32_t suspectThresholdMax = 65536;
		uint64_t heapTargetBytes = 1u << 20;   // memory pressure arm...
		uint64_t allocThresholdBytes = 1u << 20;  // ...needs both
		uint64_t maxInterval = 65536;      // starvation bound: never wait longer
	};

private:
	enum class CycleTrigger {
		None,
		SuspectPressure,
		HeapPressure,
		Starvation,
		FixedInterval,
	};

	loxis::aarc::ObjHandle current(uint32_t index) const;
	void afterOp();
	CycleTrigger cycleTrigger() const;
	void collectCyclesNow();
	void noteThreshold(uint32_t next);

	AarcCellSource m_cells;
	loxis::aarc::RefTable m_refs;
	loxis::aarc::AarcEngineT<AarcCellSource> m_engine;
	CyclePolicy m_policy;
	bool m_adaptive = true;
	uint32_t m_fixedInterval = 1024;
	uint32_t m_suspectThreshold = 4096;
	uint64_t m_opsSinceCycle = 0;
	uint64_t m_allocBytesSinceCycle = 0;
	uint64_t m_opsTotal = 0;
	uint32_t m_thresholdMinSeen = 4096;
	uint32_t m_thresholdMaxSeen = 4096;
	std::vector<uint32_t> m_scanNs;
	std::vector<uint32_t> m_scanVisited;
	std::vector<uint32_t> m_scanTriggerSuspects;  // stack entries at trigger
	std::vector<uint32_t> m_scanLiveSuspects;     // of those, actually scanned
	std::vector<ScanSample> m_scanTrace;
	std::vector<std::pair<uint64_t, uint32_t>> m_thresholdTrace;
};

std::unique_ptr<GCBackend> makeGCBackend(GCMode mode, Heap& heap);
bool parseGCMode(const std::string& s, GCMode& out);
