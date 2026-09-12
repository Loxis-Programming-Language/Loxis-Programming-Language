// AARC arc layer: the collection engine.
//
// The engine owns the authoritative object graph + refcounts (via RefTable).
// It is single-writer: in async mode only the background worker thread calls
// process()/sweepZero(); in sync mode only the test/driver thread does.
//
// Reclamation discipline (Todo_AddGC.md 1.2 / 1.4):
//   * process() applies one message and only *marks* cells whose refcount has
//     reached zero; it never frees inline.
//   * sweepZero() frees marked cells, and must only be called at a drain
//     point — a moment at which every mutator queue has been consumed up to a
//     common snapshot (see runtime.hpp).  At that moment every reference
//     change that happened-before the snapshot is already reflected in the
//     table, so a refcount of 0 is final and freeing is safe.
//   * Freeing one cell recursively releases its outgoing edges (its outgoing
//     targets get their incoming edge count decremented, which may mark them
//     for reclamation too) — never a bare free().
//
// Stale messages (a handle whose slot was recycled since the message was
// enqueued) are detected via the embedded generation and dropped, never
// applied to a different object (ABA protection).

#pragma once

#include "../core/cell.hpp"
#include "../core/gc_message.hpp"
#include "../cycle/cycle_detector.hpp"
#include "ref_table.hpp"
#include "local_components.hpp"
#include <cstdint>
#include <vector>
#include <cassert>

// Diagnostic counters (messages processed, cells reclaimed) are compiled out
// in clean Release measurements via AARC_ENABLE_STATS=0; they are plain
// single-writer counters here, so the only real cost is a hot increment.
#ifndef AARC_ENABLE_STATS
#define AARC_ENABLE_STATS 1
#endif

// Regime-invariant checker (adaptive-rc design doc §7 M2 hard gates).  Off by
// default; enable in checked builds once a pool can hold mixed regimes.
#ifndef AARC_CHECK_INVARIANTS
#define AARC_CHECK_INVARIANTS 0
#endif

// Traversal/scan counters for the T8 locating diagnostic: how much of the pool
// does a LOCAL engine actually visit per collect, and does that scale with the
// thread count?  Off by default; bench_diag_i compiles with it on.
#ifndef AARC_INSTRUMENT
#define AARC_INSTRUMENT 0
#endif

namespace loxis { namespace aarc {

// `Cells` is the object storage the engine accounts over.  CellPool is the
// in-tree implementation; a host VM can substitute its own storage (see
// AarcCellSource in the Loxis VM) so that collector metadata and object storage
// stay decoupled -- the algorithm below never touches payload bytes.
template <class Cells>
class AarcEngineT {
public:
    // Authority context: which slots may THIS engine touch?  (design doc §7 M2)
    //   * Shared engine : only regime == SHARED slots
    //   * Local engine  : only regime == LOCAL slots whose owner == owner_
    // Multiple engines share one RefTable (refs) + one Cells source but own
    // disjoint slot sets, so "one slot, one writer" holds across engines.
    AarcEngineT(Cells& pool, RefTable& refs, Regime kind = Regime::Shared,
                OwnerToken owner = kNoOwnerToken)
        : pool_(pool),
          tab_(refs),
          kind_(kind),
          owner_(owner),
          zeroMark_(pool.capacity(), 0),
          suspectMark_(pool.capacity(), 0),
          detector_(pool.capacity()) {}

    AarcEngineT(const AarcEngineT&) = delete;
    AarcEngineT& operator=(const AarcEngineT&) = delete;

    Regime kind() const { return kind_; }
    OwnerToken owner() const { return owner_; }
    RefTable& table() { return tab_; }

    // Mark this engine as coexisting with another authority over the same pool
    // (adaptive runtimes, M2).  Disables the whole-pool invariant scan inside
    // sweep() — that scan walks every live slot's RefTable row, which is only
    // safe while this engine is the *sole* writer.  Checked builds verify the
    // invariant explicitly at a fully-quiesced point instead (see tests).
    void setMixed(bool m) { mixed_ = m; }
    bool mixed() const { return mixed_; }
    // Conservative scheme: the owner's LOCAL component store (null otherwise).
    void setComponents(ConservativeComponents* comps) { comps_ = comps; }

    // Authority gate for a handle this engine is about to write.  For a LOCAL
    // engine this is regime == LOCAL AND owner matches — a foreign-owner LOCAL
    // object is off-limits even though the regime bits look identical.
    bool canTouch(ObjHandle h) const {
        if (!pool_.valid(h)) return false;
        const Regime r = pool_.regimeOf(h);
        if (r == Regime::Promoting) return false;
        if (kind_ == Regime::Shared) return r == Regime::Shared;
        return r == Regime::Local && pool_.ownerTokenAt(h.index) == owner_;
    }

    // assert()-style gate on the authority precondition.  Compiled out unless
    // AARC_CHECK_INVARIANTS (checked builds); the M2 dispatch layer is expected
    // to enforce the same rule structurally before an op is even offered to us.
    void touchGuard(ObjHandle h) const {
        (void)h;
#if AARC_CHECK_INVARIANTS
        assert(canTouch(h) && "engine touched a slot outside its authority");
#endif
    }

    // Apply one message (worker / sync-driver only).
    void process(const GCMessage& m) {
#if AARC_ENABLE_STATS
        ++msgsProcessed_;
#endif
        // Authority gate (checked builds only): every *live* slot this engine
        // is about to mutate must lie inside its authority.  A dead handle is
        // handled by the liveness checks below (staleDropped) and must not trip
        // the gate, so only live slots are asserted.
#if AARC_CHECK_INVARIANTS
        if (live(m.b)) touchGuard(m.b);
        if (live(m.a)) touchGuard(m.a);
#endif
        switch (m.op) {
            case GCOp::RootAdd:
                if (live(m.b)) {
                    tab_.addRoot(m.b.index, m.count);
                } else {
                    ++staleDropped_;
                }
                break;

            case GCOp::RootRemove: {
                if (!live(m.b)) { ++staleDropped_; break; }
                if (!tab_.subRoot(m.b.index, m.count)) { ++rootUnderflow_; break; }
                if (tab_.total(m.b.index) == 0) markZero(m.b.index);
                else if (tab_.hasOutgoing(m.b.index)) markSuspect(m.b.index);
                break;
            }

            case GCOp::RootCheck:
                // Net-zero fold of a balanced RootAdd/RootRemove pair: the count
                // is already correct, but the marking the folded remove would
                // have performed must still happen (it is what keeps a cell that
                // just lost its last reference eligible for reclamation).
                if (!live(m.b)) { ++staleDropped_; break; }
                if (tab_.total(m.b.index) == 0) markZero(m.b.index);
                else if (tab_.hasOutgoing(m.b.index)) markSuspect(m.b.index);
                break;

            case GCOp::EdgeAdd: {
                // source must be live; a dead dst means the app stored a
                // dangling field (its own bug) — drop, count nothing.
                if (!live(m.a) || !live(m.b)) { ++staleDropped_; break; }
                tab_.addEdgeTo(m.b.index);
                tab_.addOut(m.a, m.b);
                break;
            }

            case GCOp::EdgeRemove: {
                if (!live(m.a)) { ++staleDropped_; break; }
                // Remove the edge from the graph first.  If it was not there
                // the reference was never recorded, so the target's count must
                // NOT be decremented (bookkeeping stays consistent).
                if (!tab_.removeOut(m.a, m.b)) { ++edgeUnderflow_; break; }
                if (!live(m.b)) { ++staleDropped_; break; }   // dst recycled
                if (!tab_.subEdgeTo(m.b.index)) { ++edgeUnderflow_; break; }
                if (tab_.total(m.b.index) == 0) markZero(m.b.index);
                else if (tab_.hasOutgoing(m.b.index)) markSuspect(m.b.index);
                break;
            }

            case GCOp::Terminate:
                break;
        }
    }

    // Reclaim every marked-zero cell.  MUST be called only at a drain point.
    // Cascades through outgoing edges (nested release).  Visits only the
    // engine's OWN accumulated zero candidates (never a whole-pool scan).
    void sweepZero() {
        while (!zeroStack_.empty()) {
#if AARC_INSTRUMENT
            ++inZeroVisited_;
#endif
            const uint32_t i = zeroStack_.back();
            zeroStack_.pop_back();
            zeroMark_[i] = 0;
            if (!pool_.isLiveAt(i)) continue;        // already gone
            if (tab_.total(i) != 0) continue;        // revived within the drain
            reclaimCell(i);
        }
    }

    // Full drain-point collection: reclaim refcount-zero cells, then reclaim
    // cyclic garbage (cells whose refcount is held only by their own cycle),
    // then drain any new zero candidates created by the cyclic pass.
    // MUST be called only at a drain point.
    //
    // The two passes feed each other and must be driven to a fixed point:
    //   * freeing a zero cell releases its outgoing edges, which can remove the
    //     last external edge into a cycle and mark that cycle suspect;
    //   * reclaiming a cycle frees its members, which can drop other cells to
    //     zero.
    // A single sweepZero AFTER collectCycles therefore leaves the suspects that
    // very sweep just created unprocessed -- they would wait for the next drain
    // point, i.e. leak forever if the program then quiesces.  (Found by the
    // M10a differential stream: a self-loop component hanging off a dying tree
    // was marked suspect by the final sweepZero and never collected.)
    void sweep() {
#if AARC_INSTRUMENT
        ++inSweeps_;
#endif
        for (;;) {
            sweepZero();
            if (suspectStack_.empty()) break;
            collectCycles();     // drains the suspects; may mark new ones
        }
#if AARC_CHECK_INVARIANTS
        // Whole-pool scan; only safe when this engine is the sole RefTable
        // writer (see setMixed / design doc §7 M2).
        if (!mixed_)
            assert(regimeInvariantOk() && "cross-regime edge or live PROMOTING slot at quiesce");
#endif
        assertActiveSuspects();
    }

    // True when, at this (quiescent) point: no live edge crosses between
    // regimes and no slot is left PROMOTING.  Design doc §3/§7 M2 hard gate.
    // O(live * out-degree); call only in checked builds / tests.
    bool regimeInvariantOk() const {
        bool ok = true;
        pool_.forEachLive([&](ObjHandle h) {
            const Regime r = pool_.regimeOf(h);
            if (r == Regime::Promoting) { ok = false; return; }
            for (const ObjHandle& t : tab_.outs(h.index)) {
                if (pool_.valid(t) && pool_.regimeOf(t) != r) { ok = false; return; }
            }
        });
        return ok;
    }

    // --- cyclic garbage collection (drain point only) ----------------------

    // Find suspect components whose refcount is entirely internal to the
    // component (ECR == 0) and reclaim them as a unit.
    //
    // Completeness: a component only *becomes* garbage when the last external
    // reference into it is removed, and every such removal marks its target as
    // a suspect (see process() and reclaimCell()/reclaimCycleMember(), which
    // also mark suspects when reclamation orphans a cycle).  Scanning from all
    // pending suspects covers every newly-orphaned component.
    //
    // Repeated passes: reclaiming one garbage component can orphan another one
    // behind it (A<->B -> C<->D); the pass marks the newly-orphaned suspects,
    // so loop until a pass reclaims nothing.
    void collectCycles() {
        while (collectCyclePass()) {
        }
    }

private:
    // One scan+reclaim pass.  Returns true if any cell was reclaimed.
    bool collectCyclePass() {
#if AARC_INSTRUMENT
        ++inCyclePasses_;
#endif
        roots_.clear();
        // suspectStack_ holds bare slot indexes, so a slot that was marked
        // suspect and then reclaimed can linger here.  reclaimCell /
        // reclaimCycleMember clear suspectMark_, so requiring the mark at scan
        // time drops stale entries even when the slot was reused by a different
        // object (index ABA across regimes).
        for (uint32_t i : suspectStack_) {
            if (suspectMark_[i] && pool_.isLiveAt(i) && tab_.total(i) > 0) {
#if AARC_CHECK_INVARIANTS
                assert(canTouch(ObjHandle{i, pool_.generationOf(i)}) &&
                       "cycle scan left its regime");
#endif
                roots_.push_back(i);
            }
        }
        suspectStack_.clear();
        lastScanSuspects_ = roots_.size();
        if (roots_.empty()) return false;

#if AARC_INSTRUMENT
        ++inTarjans_;
#endif
        ++tarjanRuns_;
        const uint32_t k = detector_.findComponents(pool_, tab_, roots_, visited_);
        tarjanVisited_ += visited_.size();
        lastScanVisited_ = visited_.size();
        lastScanRoots_ = roots_.size();
        if (k == 0) {
            leakNodes_ += visited_.size();
            return false;
        }

        // Per-component ECR:  sum of root refs + external incoming edges.
        // external incoming edges = edge(i) summed over members, minus the
        // edges whose source is another member (internal).
        sumRoot_.assign(k, 0);
        sumEdge_.assign(k, 0);
        internal_.assign(k, 0);
        for (uint32_t i : visited_) {
            const uint32_t s = detector_.sccOf(i);
            sumRoot_[s] += tab_.root(i);
            sumEdge_[s] += tab_.edge(i);
        }
        for (uint32_t i : visited_) {
            const uint32_t s = detector_.sccOf(i);
            for (const ObjHandle& t : tab_.outs(i)) {
                if (pool_.valid(t) && detector_.sccOf(t.index) == s)
                    ++internal_[s];
            }
        }
        reclaim_.assign(k, false);
        for (uint32_t s = 0; s < k; ++s) {
            reclaim_[s] = (sumRoot_[s] == 0 && sumEdge_[s] == internal_[s]);
        }
        // Suspect flags for everything we looked at are consumed now; a live
        // component that still has external refs will be re-marked by the next
        // removal that affects it.
        for (uint32_t i : visited_)
            clearSuspect(i);

        // Reclaim each garbage component as a unit.
        bool any = false;
#ifdef AARC_CONSERVATIVE
        // Forward-only table: rows are independent, so members can be freed one
        // by one (each releases its own outgoing edges and resets its row).
        for (uint32_t i : visited_) {
            const uint32_t s = detector_.sccOf(i);
            if (reclaim_[s] && pool_.isLiveAt(i)) {
                reclaimCycleMember(i, s);
                any = true;
            }
        }
#else
        // Reverse table: all edge records in a component must be detached before
        // any member row is reset, because intrusive incoming lists would still
        // point at later members.  Bucket members by component in ONE pass over
        // visited_: rescanning visited_ per component is O(V * components), which
        // turns a single large scan (one that waited for a big backlog)
        // quadratic -- measured as a 44x slowdown on the 400k-object cycle
        // workload before this fix.
        if (membersOf_.size() < k) membersOf_.resize(k);
        for (uint32_t s = 0; s < k; ++s) membersOf_[s].clear();
        for (uint32_t i : visited_) {
            if (pool_.isLiveAt(i)) membersOf_[detector_.sccOf(i)].push_back(i);
        }
        for (uint32_t s = 0; s < k; ++s) {
            if (reclaim_[s] && !membersOf_[s].empty()) {
                reclaimCycleComponent(membersOf_[s], s);
                any = true;
            }
        }
#endif
        detector_.release(visited_);
        releaseNodes_ += visited_.size();
        return any;
    }

public:
    // --- diagnostics / queries (test & driver use) -------------------------

    // True when the engine holds no refcount for the cell (dead or unreferenced).
    int64_t refcountOf(ObjHandle h) const {
        if (!pool_.valid(h)) return -1;
        return tab_.total(h.index);
    }
    int64_t rootOf(ObjHandle h) const {
        if (!pool_.valid(h)) return -1;
        return tab_.root(h.index);
    }
    int64_t edgeOf(ObjHandle h) const {
        if (!pool_.valid(h)) return -1;
        return tab_.edge(h.index);
    }
    size_t outgoingCount(ObjHandle h) const {
        if (!pool_.valid(h)) return 0;
        return tab_.outs(h.index).size();
    }
    size_t incomingCount(ObjHandle h) const {
        if (!pool_.valid(h)) return 0;
#ifdef AARC_CONSERVATIVE
        return 0;   // forward-only scheme keeps no incoming list
#else
        return tab_.ins(h.index).size();
#endif
    }
    uint64_t liveEdgeRecords() const { return tab_.liveEdges(); }
    bool isSuspect(ObjHandle h) const {
        return pool_.valid(h) && suspectMark_[h.index];
    }
    // Lock-free variant (worker-idle context only) for diagnostics that are
    // already holding the pool lock (e.g. inside CellPool::forEachLive).
    bool suspectBit(uint32_t i) const { return suspectMark_[i]; }

    uint64_t staleDropped() const { return staleDropped_; }
    uint64_t rootUnderflow() const { return rootUnderflow_; }
    uint64_t edgeUnderflow() const { return edgeUnderflow_; }
    uint64_t totalErrors() const {
        return staleDropped_ + rootUnderflow_ + edgeUnderflow_;
    }

    // Collection counters (worker writes; safe to read once quiesced).
#if AARC_ENABLE_STATS
    uint64_t msgsProcessed() const { return msgsProcessed_; }
    uint64_t cellsReclaimedZero() const { return reclaimedZero_; }
    uint64_t cellsReclaimedCycle() const { return reclaimedCycle_; }
#else
    uint64_t msgsProcessed() const { return 0; }
    uint64_t cellsReclaimedZero() const { return 0; }
    uint64_t cellsReclaimedCycle() const { return 0; }
#endif

    size_t zeroCandidates() const { return zeroStack_.size(); }
    bool hasZeroCandidates() const { return !zeroStack_.empty(); }
    bool isZeroMarked(uint32_t i) const { return zeroMark_[i]; }

    // Cycle-scan instrumentation, always on: these are single-writer counters
    // (one increment per Tarjan pass) and the adaptive trigger needs them to
    // decide how often a scan is worth running.
    uint64_t tarjanRuns() const { return tarjanRuns_; }
    uint64_t tarjanVisited() const { return tarjanVisited_; }
    // Suspects scanned by the most recent pass (the pass consumes the stack).
    size_t lastScanSuspects() const { return lastScanSuspects_; }
    uint64_t lastScanSkippedDisc() const { return detector_.skippedAlreadyDiscovered(); }
    uint64_t lastScanSkippedDead() const { return detector_.skippedDead(); }
    size_t lastScanVisited() const { return lastScanVisited_; }
    size_t lastScanRoots() const { return lastScanRoots_; }
    uint64_t releasedNodes() const { return releaseNodes_; }
    uint64_t leakedScanNodes() const { return leakNodes_; }

    // Traversal counters (AARC_INSTRUMENT diagnostic builds only).
#if AARC_INSTRUMENT
    uint64_t instrumentSweeps() const { return inSweeps_; }
    uint64_t instrumentZeroVisited() const { return inZeroVisited_; }
    uint64_t instrumentCyclePasses() const { return inCyclePasses_; }
    uint64_t instrumentTarjans() const { return inTarjans_; }
#else
    uint64_t instrumentSweeps() const { return 0; }
    uint64_t instrumentZeroVisited() const { return 0; }
    uint64_t instrumentCyclePasses() const { return 0; }
    uint64_t instrumentTarjans() const { return 0; }
#endif

    // Number of suspects awaiting the next cyclic scan (test diagnostics).
    // Distinct currently-marked suspects -- the number the adaptive trigger
    // must use. The lazy stack below is a superset (stale + duplicate entries).
    size_t pendingSuspects() const { return activeSuspects_; }
    size_t activeSuspects() const { return activeSuspects_; }
    size_t suspectStackEntries() const { return suspectStack_.size(); }
    size_t staleSuspectEntries() const { return suspectStack_.size() - activeSuspects_; }

    // Debug checkpoint: recount marks by scanning the slot array and compare
    // with the maintained counter. O(capacity); call at quiesce/test points,
    // never on the hot path.
    void assertActiveSuspects() const {
#ifndef NDEBUG
        size_t recount = 0;
        for (size_t i = 0; i < suspectMark_.size(); ++i) {
            if (suspectMark_[i]) ++recount;
        }
        assert(recount == activeSuspects_ && "activeSuspects_ out of sync");
#endif
    }

#ifdef AARC_TEST_HOOKS
    void markSuspectForTest(uint32_t i) { setSuspect(i); }
    void clearSuspectForTest(uint32_t i) { clearSuspect(i); }
#endif

    // Promotion transfers RefTable authority without moving rows.  Remove the
    // transferred indexes from this engine's private candidate scratch so a
    // later LOCAL sweep cannot revisit SHARED cells.
    void forgetPromoted(const std::vector<uint32_t>& indexes) {
        for (uint32_t i : indexes) {
            zeroMark_[i] = 0;
            clearSuspect(i);
        }
        compactMarks(zeroStack_, zeroMark_);
        compactMarks(suspectStack_, suspectMark_);
    }

private:
    static void compactMarks(std::vector<uint32_t>& stack,
                             const std::vector<uint8_t>& marks) {
        size_t write = 0;
        for (uint32_t i : stack)
            if (marks[i]) stack[write++] = i;
        stack.resize(write);
    }
    bool live(ObjHandle h) const { return pool_.valid(h); }

    void markZero(uint32_t i) {
        if (!zeroMark_[i]) {
            zeroMark_[i] = 1;
            zeroStack_.push_back(i);
        }
    }
    // Suspect marking goes through these two only: the lazy stack is a superset
    // (it keeps stale and duplicate entries, filtered at scan time), so the
    // *live* count must be maintained separately or the adaptive trigger would
    // be reading the size of a work log instead of the number of distinct
    // suspects. Measured inflation was ~56x (1390 stack entries, 25 distinct).
    void setSuspect(uint32_t i) {
        if (suspectMark_[i]) return;
        suspectMark_[i] = 1;
        ++activeSuspects_;
        suspectStack_.push_back(i);
    }
    void clearSuspect(uint32_t i) {
        if (!suspectMark_[i]) return;
        suspectMark_[i] = 0;
        assert(activeSuspects_ > 0 && "suspect count underflow");
        --activeSuspects_;
    }
    void markSuspect(uint32_t i) { setSuspect(i); }

    // Free `i`, first releasing its outgoing edges so reachable objects are
    // not leaked and are reclaimed in turn (Todo_AddGC.md 1.4).
    void reclaimCell(uint32_t i) {
#if AARC_ENABLE_STATS
        ++reclaimedZero_;
#endif
        clearSuspect(i);
        const ObjHandle self{i, pool_.generationOf(i)};
        for (const ObjHandle& t : tab_.outs(i)) {
            if (!pool_.isLiveAt(t.index)) continue;
            tab_.subEdgeTo(t.index);            // edge from i no longer exists
            if (tab_.total(t.index) == 0) {
                markZero(t.index);
            } else if (tab_.hasOutgoing(t.index)) {
                // removing i may have orphaned a cycle behind t
                markSuspect(t.index);
            }
        }
        if (comps_) comps_->unlink(owner_, i);
        tab_.resetSlot(i);
        pool_.free(self);
    }

    // Reclaim a garbage SCC in two phases: first release external targets and
    // detach every member's outgoing records (including internal edges), then
    // reset/free member rows.  ECR == 0 guarantees no edge from outside points
    // into the component.
    void reclaimCycleComponent(const std::vector<uint32_t>& members,
                               uint32_t scc) {
        for (uint32_t i : members) {
#if AARC_ENABLE_STATS
            ++reclaimedCycle_;
#endif
            for (const ObjHandle& t : tab_.outs(i)) {
                if (!pool_.valid(t)) continue;
                const uint32_t idx = t.index;
                if (detector_.sccOf(idx) == scc) continue;
                tab_.subEdgeTo(idx);
                if (tab_.total(idx) == 0) {
                    markZero(idx);
                } else if (tab_.hasOutgoing(idx)) {
                    markSuspect(idx);
                }
            }
            tab_.clearOuts(i);
        }
        for (uint32_t i : members) {
            tab_.resetSlot(i);
            clearSuspect(i);
            pool_.free(ObjHandle{i, pool_.generationOf(i)});
        }
    }

    // Single-member cycle reclaim (forward-only / conservative table): each row
    // is independent, so a member can be freed as soon as its external targets
    // are released.  ECR == 0 guarantees no edge points into the component.
    void reclaimCycleMember(uint32_t i, uint32_t scc) {
#if AARC_ENABLE_STATS
        ++reclaimedCycle_;
#endif
        for (const ObjHandle& t : tab_.outs(i)) {
            if (!pool_.valid(t)) continue;
            const uint32_t idx = t.index;
            if (detector_.sccOf(idx) == scc) continue;
            tab_.subEdgeTo(idx);
            if (tab_.total(idx) == 0) {
                markZero(idx);
            } else if (tab_.hasOutgoing(idx)) {
                markSuspect(idx);
            }
        }
        if (comps_) comps_->unlink(owner_, i);
        tab_.resetSlot(i);
        clearSuspect(i);
        pool_.free(ObjHandle{i, pool_.generationOf(i)});
    }

    Cells& pool_;
    RefTable& tab_;
    Regime kind_;                  // Shared (worker) or Local (owner) engine
    OwnerToken owner_;             // owning thread for Local engines
    bool mixed_ = false;           // coexists with another authority (adaptive)
    ConservativeComponents* comps_ = nullptr;   // conservative LOCAL comp store
    std::vector<uint8_t> zeroMark_;
    std::vector<uint8_t> suspectMark_;
    std::vector<uint32_t> zeroStack_;
    std::vector<uint32_t> suspectStack_;
    std::vector<uint32_t> roots_;
    std::vector<uint32_t> visited_;
    std::vector<int64_t> sumRoot_;
    std::vector<int64_t> sumEdge_;
    std::vector<int64_t> internal_;
    std::vector<uint8_t> reclaim_;
    std::vector<std::vector<uint32_t>> membersOf_;   // per-SCC members, reused
    CycleDetectorT<Cells> detector_;

    uint64_t tarjanRuns_ = 0;
    uint64_t tarjanVisited_ = 0;
    size_t lastScanSuspects_ = 0;
    size_t lastScanVisited_ = 0;
    size_t lastScanRoots_ = 0;
    uint64_t releaseNodes_ = 0;
    uint64_t leakNodes_ = 0;
    size_t activeSuspects_ = 0;

    uint64_t staleDropped_ = 0;
    uint64_t rootUnderflow_ = 0;
    uint64_t edgeUnderflow_ = 0;
#if AARC_ENABLE_STATS
    uint64_t msgsProcessed_ = 0;
    uint64_t reclaimedZero_ = 0;
    uint64_t reclaimedCycle_ = 0;
#endif
#if AARC_INSTRUMENT
    uint64_t inSweeps_ = 0;
    uint64_t inZeroVisited_ = 0;
    uint64_t inCyclePasses_ = 0;
    uint64_t inTarjans_ = 0;
#endif
};

using AarcEngine = AarcEngineT<CellPool>;

}} // namespace loxis::aarc
