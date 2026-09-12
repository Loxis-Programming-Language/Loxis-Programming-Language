#include "GC.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ostream>

void GCBackend::dumpMetrics(std::ostream& os) const {
	os << "gc=" << name()
	   << " allocs=" << m_metrics.allocations
	   << " reclaimed=" << m_metrics.reclaimedObjects
	   << " rootAdds=" << m_metrics.rootAdds
	   << " rootRemoves=" << m_metrics.rootRemoves
	   << " edgeAdds=" << m_metrics.edgeAdds
	   << " edgeRemoves=" << m_metrics.edgeRemoves
	   << " cycleCollections=" << m_metrics.cycleCollections
	   << " cycleReclaimed=" << m_metrics.cycleObjectsReclaimed
	   << " cycleScans=" << m_metrics.cycleScans
	   << " cycleVisited=" << m_metrics.cycleScanVisited
	   << " cycleBytes=" << m_metrics.cycleReclaimedBytes
	   << " cycleNs=" << m_metrics.cycleCollectNs
	   << " cycleNsMax=" << m_metrics.cycleNsMax
	   << " cycleByPolicy=" << m_metrics.cycleObjectsByPolicy
	   << " trigSuspect=" << m_metrics.triggerSuspectPressure
	   << " trigHeap=" << m_metrics.triggerHeapPressure
	   << " trigStarve=" << m_metrics.triggerStarvation
	   << " trigFixed=" << m_metrics.triggerFixedInterval
	   << " thrFinal=" << m_metrics.thresholdFinal
	   << " thrMin=" << m_metrics.thresholdMin
	   << " thrMax=" << m_metrics.thresholdMax
	   << " thrUp=" << m_metrics.thresholdRaises
	   << " thrDown=" << m_metrics.thresholdLowers
	   << " ignoredFrees=" << m_metrics.explicitFreesIgnored
	   << " invalidOps=" << m_metrics.invalidHandleOps;
	if (m_heap) {
		const Heap::Stats& s = m_heap->stats();
		os << " live=" << s.liveObjects
		   << " peakLive=" << s.peakLiveObjects
		   << " heapBytes=" << s.bytesCommitted
		   << " liveBytes=" << s.bytesLive
		   << " reusedBytes=" << s.bytesReused
		   << " reuseAllocs=" << s.reuses;
	}
	os << "\n";
}

// ---------------------------------------------------------------- NoGC ----

void NoGCBackend::onAlloc(uint32_t) { m_metrics.allocations++; }
void NoGCBackend::rootAdd(uint32_t) { m_metrics.rootAdds++; }
void NoGCBackend::rootRemove(uint32_t) { m_metrics.rootRemoves++; }

void NoGCBackend::edgeAdd(uint32_t, uint32_t) { m_metrics.edgeAdds++; }

void NoGCBackend::edgeRemove(uint32_t, uint32_t) { m_metrics.edgeRemoves++; }

void NoGCBackend::onExplicitFree(uint32_t) { m_metrics.explicitFreesIgnored++; }

void NoGCBackend::shutdown() {
	if (!m_heap) return;
	m_metrics.reclaimedObjects = 0;
}

// ----------------------------------------------------------------- ARC ----

void ArcBackend::attach(Heap* heap) {
	GCBackend::attach(heap);
	m_rc.assign(heap->slotCount(), 0);
}

void ArcBackend::onAlloc(uint32_t handle) {
	m_metrics.allocations++;
	if (handle >= m_rc.size()) m_rc.resize(handle + 1, 0);
	m_rc[handle] = 0;
}

void ArcBackend::retain(uint32_t handle) {
	if (handle >= m_rc.size()) m_rc.resize(handle + 1, 0);
	m_rc[handle]++;
}

void ArcBackend::release(uint32_t handle) {
	if (handle >= m_rc.size() || m_rc[handle] == 0) {
		// A release with no matching retain: either a stale handle or a
		// bookkeeping hole. Counted so a silent leak/double-free cannot hide.
		m_metrics.invalidHandleOps++;
		return;
	}
	if (--m_rc[handle] == 0) reclaim(handle);
}

void ArcBackend::rootAdd(uint32_t handle) {
	m_metrics.rootAdds++;
	retain(handle);
}

void ArcBackend::rootRemove(uint32_t handle) {
	m_metrics.rootRemoves++;
	release(handle);
}

void ArcBackend::edgeAdd(uint32_t from, uint32_t to) {
	(void) from;
	m_metrics.edgeAdds++;
	retain(to);
}

void ArcBackend::edgeRemove(uint32_t from, uint32_t to) {
	(void) from;
	m_metrics.edgeRemoves++;
	release(to);
}

void ArcBackend::onExplicitFree(uint32_t) { m_metrics.explicitFreesIgnored++; }

void ArcBackend::reclaim(uint32_t root) {
	m_work.push_back(root);
	while (!m_work.empty()) {
		uint32_t handle = m_work.back();
		m_work.pop_back();

		// Children are released before the payload goes away; the slot stays
		// valid for the whole scan because nothing allocates during reclaim.
		uint32_t cells = m_heap->cellCount(handle);
		for (uint32_t i = 0; i < cells; i++) {
			auto cell = m_heap->cellAt(handle, i);
			if (cell.second != TAG_HEAP) continue;
			uint32_t child = static_cast<uint32_t>(cell.first);
			if (child >= m_rc.size() || m_rc[child] == 0) continue;
			if (--m_rc[child] == 0) m_work.push_back(child);
		}

		m_heap->freePayload(handle);
		m_metrics.reclaimedObjects++;
	}
}

void ArcBackend::shutdown() {
	if (!m_heap) return;
	// Report what reference counting could not reach: leaked self-cycles and
	// anything still rooted at exit.
	uint64_t leaked = 0;
	for (size_t h = 0; h < m_rc.size(); h++) {
		if (m_rc[h] > 0 && m_heap->valid(static_cast<uint32_t>(h))) leaked++;
	}
	m_metrics.cycleObjectsReclaimed = 0;
	(void) leaked;
}

// ---------------------------------------------------------- LOCAL-AARC ----

LocalAarcBackend::LocalAarcBackend(Heap& heap)
	: m_cells(heap),
	  m_refs(heap.slotCapacity()),
	  m_engine(m_cells, m_refs, loxis::aarc::Regime::Local, AarcCellSource::kOwner) {
	if (const char* c = std::getenv("LOXIS_AARC_CADENCE")) {
		const long v = std::strtol(c, nullptr, 10);
		if (v > 0) {
			m_adaptive = false;
			m_fixedInterval = static_cast<uint32_t>(v);
		}
	}
}

loxis::aarc::ObjHandle LocalAarcBackend::current(uint32_t index) const {
	return loxis::aarc::ObjHandle{index, m_cells.generationOf(index)};
}

void LocalAarcBackend::setCyclePolicy(const std::string& spec) {
	if (spec == "adaptive") {
		m_adaptive = true;
		return;
	}
	if (spec.rfind("fixed:", 0) == 0) {
		const long v = std::strtol(spec.c_str() + 6, nullptr, 10);
		if (v > 0) {
			m_adaptive = false;
			m_fixedInterval = static_cast<uint32_t>(v);
		}
		return;
	}
	// Unknown spec: keep whatever the constructor chose.
}

// Cheap, eager, always safe: zero-chain reclamation only walks cells whose
// count already reached zero, and it is what keeps the heap from growing.
// The expensive Tarjan scan is the part that gets a policy.
void LocalAarcBackend::afterOp() {
	++m_opsSinceCycle;
	++m_opsTotal;
	if (m_engine.hasZeroCandidates()) m_engine.sweepZero();

	const CycleTrigger reason = cycleTrigger();
	switch (reason) {
		case CycleTrigger::None:
			return;
		case CycleTrigger::SuspectPressure:
			m_metrics.triggerSuspectPressure++;
			break;
		case CycleTrigger::HeapPressure:
			m_metrics.triggerHeapPressure++;
			break;
		case CycleTrigger::Starvation:
			m_metrics.triggerStarvation++;
			break;
		case CycleTrigger::FixedInterval:
			m_metrics.triggerFixedInterval++;
			break;
	}
	collectCyclesNow();
}

LocalAarcBackend::CycleTrigger LocalAarcBackend::cycleTrigger() const {
	const size_t suspects = m_engine.pendingSuspects();
	if (suspects == 0) return CycleTrigger::None;   // nothing to scan

	if (!m_adaptive) {
		return m_opsSinceCycle >= m_fixedInterval ? CycleTrigger::FixedInterval
		                                          : CycleTrigger::None;
	}

	// 1. suspect pressure: enough candidates that a scan is likely to pay off
	if (suspects >= m_suspectThreshold) return CycleTrigger::SuspectPressure;

	// 2. memory pressure: the heap is past its target and we have allocated a
	//    meaningful amount since the last scan
	if (m_heap->bytesCommitted() >= m_policy.heapTargetBytes &&
	    m_allocBytesSinceCycle >= m_policy.allocThresholdBytes) {
		return CycleTrigger::HeapPressure;
	}

	// 3. starvation bound: a small cyclic workload may never trip 1 or 2, so
	//    guarantee a look at least this often. Without it a tiny quiet cycle
	//    lives forever.
	if (m_opsSinceCycle >= m_policy.maxInterval) return CycleTrigger::Starvation;

	return CycleTrigger::None;
}

void LocalAarcBackend::noteThreshold(uint32_t next) {
	if (next == m_suspectThreshold) return;
	if (next > m_suspectThreshold) {
		m_metrics.thresholdRaises++;
	} else {
		m_metrics.thresholdLowers++;
	}
	m_suspectThreshold = next;
	if (next < m_thresholdMinSeen) m_thresholdMinSeen = next;
	if (next > m_thresholdMaxSeen) m_thresholdMaxSeen = next;
	if (m_thresholdTrace.size() < 64) {
		m_thresholdTrace.emplace_back(m_opsTotal, next);
	}
}

void LocalAarcBackend::collectCyclesNow() {
	const uint64_t visitedBefore = m_engine.tarjanVisited();
	const uint64_t reclaimedBefore = m_engine.cellsReclaimedCycle();
	const uint64_t bytesBefore = m_heap->stats().bytesLive;
	const size_t triggerSuspects = m_engine.pendingSuspects();       // distinct
	const size_t triggerStack = m_engine.suspectStackEntries();      // raw log

	m_metrics.cycleCollections++;
	const auto t0 = std::chrono::steady_clock::now();
	m_engine.collectCycles();
	const auto t1 = std::chrono::steady_clock::now();
	const uint64_t ns = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
	m_metrics.cycleCollectNs += ns;
	if (ns > m_metrics.cycleNsMax) m_metrics.cycleNsMax = ns;
	// Cycle reclaim frees payloads immediately, so the live-byte drop across the
	// call is exactly what the cycle pass handed back to the allocator.
	m_metrics.cycleReclaimedBytes += bytesBefore - m_heap->stats().bytesLive;

	m_engine.sweepZero();   // drain zeros the cycle reclaim created

	const uint64_t visited = m_engine.tarjanVisited() - visitedBefore;
	const uint64_t reclaimed = m_engine.cellsReclaimedCycle() - reclaimedBefore;
	m_metrics.cycleScans = m_engine.tarjanRuns();
	m_metrics.cycleScanVisited = m_engine.tarjanVisited();
	m_metrics.cycleObjectsByPolicy += reclaimed;
	m_metrics.suspectBacklog = m_engine.pendingSuspects();
	if (m_scanNs.size() < (1u << 20)) {
		m_scanNs.push_back(static_cast<uint32_t>(std::min<uint64_t>(ns, 0xFFFFFFFFu)));
		m_scanVisited.push_back(static_cast<uint32_t>(std::min<uint64_t>(visited, 0xFFFFFFFFu)));
		m_scanTriggerSuspects.push_back(
		    static_cast<uint32_t>(std::min<size_t>(triggerSuspects, 0xFFFFFFFFu)));
		m_scanLiveSuspects.push_back(static_cast<uint32_t>(
		    std::min<size_t>(m_engine.lastScanSuspects(), 0xFFFFFFFFu)));
	}
	if (m_scanTrace.size() < 48) {
		auto u32 = [](uint64_t v) { return static_cast<uint32_t>(std::min<uint64_t>(v, 0xFFFFFFFFu)); };
		m_scanTrace.push_back(ScanSample{
		    m_opsTotal, u32(triggerSuspects), u32(m_engine.lastScanSuspects()),
		    u32(visited), u32(reclaimed), m_suspectThreshold, u32(triggerStack),
		    u32(m_engine.lastScanRoots()), u32(m_engine.lastScanVisited()),
		    u32(m_engine.lastScanSkippedDisc()), u32(m_engine.lastScanSkippedDead())});
	}

	// Yield feedback: a scan that visits a lot and reclaims nothing means we
	// are scanning too often; a dense yield means scan sooner. Everything
	// between is left alone (hysteresis).
	if (m_adaptive && visited >= kYieldMinVisited) {
		if (reclaimed == 0) {
			noteThreshold(std::min<uint32_t>(m_suspectThreshold * 2,
			                                 m_policy.suspectThresholdMax));
		} else if (reclaimed * 4 >= visited) {
			noteThreshold(std::max<uint32_t>(m_suspectThreshold / 2,
			                                 m_policy.suspectThresholdMin));
		}
	}

	m_opsSinceCycle = 0;
	m_allocBytesSinceCycle = 0;
}

void LocalAarcBackend::onAlloc(uint32_t handle) {
	m_metrics.allocations++;
	if (m_heap && handle < m_heap->slotCount()) {
		m_allocBytesSinceCycle +=
		    static_cast<uint64_t>(m_heap->slot(handle).cells) * Heap::SLOT_SIZE;
	}
	// No engine call: the slot's RefTable row was zeroed by the constructor
	// (fresh slot) or by resetSlot() when the collector freed it.  Deliberately
	// no sweep here — a freshly allocated object has no root yet, and a sweep
	// would be entitled to reclaim it.
	(void) handle;
}

void LocalAarcBackend::rootAdd(uint32_t handle) {
	m_metrics.rootAdds++;
	m_engine.process({loxis::aarc::GCOp::RootAdd, {}, current(handle), 1});
	afterOp();
}

void LocalAarcBackend::rootRemove(uint32_t handle) {
	m_metrics.rootRemoves++;
	m_engine.process({loxis::aarc::GCOp::RootRemove, {}, current(handle), 1});
	afterOp();
}

void LocalAarcBackend::edgeAdd(uint32_t from, uint32_t to) {
	m_metrics.edgeAdds++;
	m_engine.process({loxis::aarc::GCOp::EdgeAdd, current(from), current(to), 1});
	afterOp();
}

void LocalAarcBackend::edgeRemove(uint32_t from, uint32_t to) {
	m_metrics.edgeRemoves++;
	m_engine.process({loxis::aarc::GCOp::EdgeRemove, current(from), current(to), 1});
	afterOp();
}

void LocalAarcBackend::onExplicitFree(uint32_t) { m_metrics.explicitFreesIgnored++; }

void LocalAarcBackend::shutdown() {
	m_engine.sweep();   // final drain point: what is still rooted at exit stays
	m_metrics.reclaimedObjects = m_engine.cellsReclaimedZero();
	m_metrics.cycleObjectsReclaimed = m_engine.cellsReclaimedCycle();
	m_metrics.cycleScans = m_engine.tarjanRuns();
	m_metrics.cycleScanVisited = m_engine.tarjanVisited();
	m_metrics.suspectBacklog = m_engine.pendingSuspects();
	m_metrics.thresholdFinal = m_suspectThreshold;
	m_metrics.thresholdMin = m_thresholdMinSeen;
	m_metrics.thresholdMax = m_thresholdMaxSeen;
}

namespace {
uint32_t pctOf(std::vector<uint32_t> v, double p) {
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	const size_t i = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
	return v[i];
}
} // namespace

void LocalAarcBackend::dumpMetrics(std::ostream& os) const {
	GCBackend::dumpMetrics(os);
	os << "  aarc: policy=" << (m_adaptive ? "adaptive" : "fixed")
	   << " fixedInterval=" << m_fixedInterval
	   << " suspectThreshold=" << m_suspectThreshold
	   << " opsSinceCycle=" << m_opsSinceCycle
	   << " activeSuspects=" << m_engine.activeSuspects()
	   << " stackEntries=" << m_engine.suspectStackEntries()
	   << " staleEntries=" << m_engine.staleSuspectEntries()
	   << " lastScanSuspects=" << m_engine.lastScanSuspects() << "\n";
	os << "  pause_ns: p50=" << pctOf(m_scanNs, 0.50)
	   << " p95=" << pctOf(m_scanNs, 0.95)
	   << " p99=" << pctOf(m_scanNs, 0.99)
	   << " max=" << m_metrics.cycleNsMax
	   << "  visited: p50=" << pctOf(m_scanVisited, 0.50)
	   << " p95=" << pctOf(m_scanVisited, 0.95)
	   << " p99=" << pctOf(m_scanVisited, 0.99)
	   << " max=" << (m_scanVisited.empty() ? 0u : *std::max_element(
	                      m_scanVisited.begin(), m_scanVisited.end())) << "\n";
	os << "  suspects: trigger_p50=" << pctOf(m_scanTriggerSuspects, 0.50)
	   << " trigger_max=" << (m_scanTriggerSuspects.empty() ? 0u : *std::max_element(
	                              m_scanTriggerSuspects.begin(), m_scanTriggerSuspects.end()))
	   << " live_p50=" << pctOf(m_scanLiveSuspects, 0.50)
	   << " live_max=" << (m_scanLiveSuspects.empty() ? 0u : *std::max_element(
	                          m_scanLiveSuspects.begin(), m_scanLiveSuspects.end())) << "\n";
	os << "  scanTrace=ops:trig:live:vis:rcl:thr\n";
	for (const ScanSample& s : m_scanTrace) {
		os << "    " << s.ops << ":" << s.trigger << ":" << s.live << ":" << s.visited
		   << ":" << s.reclaimed << ":" << s.threshold
		   << " stack=" << s.stackAtTrigger
		   << " engRoots=" << s.engRoots << " engVisited=" << s.engVisited
		   << " skipDisc=" << s.skipDisc << " skipDead=" << s.skipDead << "\n";
	}
	os << "  detector: visitedTotal=" << m_engine.tarjanVisited()
	   << " released=" << m_engine.releasedNodes()
	   << " leakedAtZeroComps=" << m_engine.leakedScanNodes() << "\n";
	os << "  thrTrace=";
	for (size_t i = 0; i < m_thresholdTrace.size(); i++) {
		if (i) os << ",";
		os << m_thresholdTrace[i].first << ":" << m_thresholdTrace[i].second;
	}
	os << "\n";
}

// ------------------------------------------------------------------------

std::unique_ptr<GCBackend> makeGCBackend(GCMode mode, Heap& heap) {
	std::unique_ptr<GCBackend> backend;
	switch (mode) {
		case GCMode::ARC:
			backend = std::make_unique<ArcBackend>();
			break;
		case GCMode::AARCLocal:
			backend = std::make_unique<LocalAarcBackend>(heap);
			break;
		case GCMode::NoGC:
		default:
			backend = std::make_unique<NoGCBackend>();
			break;
	}
	backend->attach(&heap);
	return backend;
}

bool parseGCMode(const std::string& s, GCMode& out) {
	if (s == "nogc" || s == "none") { out = GCMode::NoGC; return true; }
	if (s == "arc") { out = GCMode::ARC; return true; }
	if (s == "aarc" || s == "aarc-local") { out = GCMode::AARCLocal; return true; }
	return false;
}
