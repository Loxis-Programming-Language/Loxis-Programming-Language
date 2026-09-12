// GC benchmark harness.
//
// Design constraints, learned the hard way on this box (see the AARC M12 notes):
//   * Compile ONCE, run the same Chunk under every backend.  Otherwise the
//     comparison measures the compiler, not the collector.
//   * Time only vm.run(); JIT build, VM construction and the final teardown
//     collection are outside the measured region.
//   * Alternate the backend order every rep.  A fixed order lets within-run
//     drift (turbo, thermal, scheduler) masquerade as a backend difference and
//     has produced sign flips in this project before.
//   * Report median + the full band.  Single-run numbers on this machine swing
//     by up to 2x.
//   * Refuse to report a truncated run: if the instruction cap was hit, the
//     workload never finished and the number is meaningless.

#include "v2/driver/Driver.hpp"
#include "vm/VM.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BackendSpec {
	const char* name;
	GCMode mode;
};

const std::vector<BackendSpec> kAllBackends = {
	{"nogc", GCMode::NoGC},
	{"arc", GCMode::ARC},
	{"aarc", GCMode::AARCLocal},
};

struct Sample {
	double ms = 0.0;
	GCMetrics gc;
	Heap::Stats heap;
	bool truncated = false;
	uint64_t instrs = 0;
	std::string output;   // the program's own stdout, kept for a cross-check
};

Sample runOnce(const Chunk& proto, GCMode mode, uint32_t slots, int maxInstr,
               const std::string& cyclePolicy) {
	VM vm(Chunk(proto), false, mode, slots);
	if (!cyclePolicy.empty()) vm.setCyclePolicy(cyclePolicy);
	vm.run(0);   // build the JIT outside the measured region

	// Swallow the program's stdout so the report stays a table; keep it to
	// verify every backend produced the same result.
	std::ostringstream captured;
	std::streambuf* realBuf = std::cout.rdbuf(captured.rdbuf());

	const auto t0 = std::chrono::steady_clock::now();
	vm.run(maxInstr);
	const auto t1 = std::chrono::steady_clock::now();

	std::cout.rdbuf(realBuf);

	Sample s;
	s.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	s.truncated = vm.truncated();
	s.instrs = vm.instructionsExecuted();
	s.output = captured.str();

	vm.finishGC();   // teardown: not part of the timed region
	s.gc = vm.gcMetrics();
	s.heap = vm.heapStats();
	return s;
}

double percentile(const std::vector<double>& sorted, double p) {
	if (sorted.empty()) return 0.0;
	const double idx = p * static_cast<double>(sorted.size() - 1);
	const size_t lo = static_cast<size_t>(idx);
	const size_t hi = std::min(lo + 1, sorted.size() - 1);
	const double frac = idx - static_cast<double>(lo);
	return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

double median(std::vector<double> v) {
	if (v.empty()) return 0.0;
	std::sort(v.begin(), v.end());
	return percentile(v, 0.5);
}

} // namespace

int main(int argc, char** argv) {
	std::vector<std::string> files;
	std::vector<BackendSpec> backends;
	int reps = 11;
	int warmup = 3;
	uint32_t slots = 0;          // 0 = backend default
	int maxInstr = 2000000000;
	std::string cyclePolicy;     // "adaptive" | "fixed:N" | empty = backend default

	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		if (arg.rfind("--reps=", 0) == 0) reps = std::stoi(arg.substr(7));
		else if (arg.rfind("--warmup=", 0) == 0) warmup = std::stoi(arg.substr(9));
		else if (arg.rfind("--slots=", 0) == 0) slots = static_cast<uint32_t>(std::stoul(arg.substr(8)));
		else if (arg.rfind("--instr=", 0) == 0) maxInstr = std::stoi(arg.substr(8));
		else if (arg.rfind("--aarc-cycle=", 0) == 0) cyclePolicy = arg.substr(13);
		else if (arg.rfind("--backends=", 0) == 0) {
			std::string list = arg.substr(11);
			size_t start = 0;
			while (start <= list.size()) {
				const size_t comma = list.find(',', start);
				const std::string name = list.substr(start, comma == std::string::npos
				                                                ? std::string::npos
				                                                : comma - start);
				bool found = false;
				for (const auto& b : kAllBackends) {
					if (name == b.name) { backends.push_back(b); found = true; }
				}
				if (!found) {
					std::cerr << "unknown backend: " << name << "\n";
					return 1;
				}
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
		} else if (!arg.empty() && arg[0] == '-') {
			std::cerr << "unknown flag: " << arg << "\n";
			return 1;
		} else {
			files.push_back(arg);
		}
	}
	if (backends.empty()) backends = kAllBackends;
	if (files.empty()) {
		std::cerr << "usage: bench_gc <file.lx>... [--reps=N] [--warmup=N] [--slots=N]"
		             " [--instr=N] [--aarc-cycle=adaptive|fixed:N]"
		             " [--backends=nogc,arc,aarc]\n";
		return 1;
	}

	for (const std::string& file : files) {
		loxis::v2::Driver driver;
		Chunk proto;
		if (driver.compileToChunk(file, false, proto) != 0) {
			std::cerr << "compile failed: " << file << "\n";
			return 1;
		}

		// backend name -> samples (warmup runs discarded)
		std::vector<std::vector<Sample>> samples(backends.size());

		for (int rep = 0; rep < warmup + reps; rep++) {
			// Alternate the order every rep so within-run drift cannot favour a
			// backend consistently.
			std::vector<size_t> order(backends.size());
			for (size_t i = 0; i < order.size(); i++) order[i] = i;
			if (rep % 2 == 1) std::reverse(order.begin(), order.end());

			for (size_t idx : order) {
				Sample s = runOnce(proto, backends[idx].mode, slots, maxInstr, cyclePolicy);
				if (rep >= warmup) samples[idx].push_back(s);
			}
		}

		std::cout << "\n=== " << file << "  (" << reps << " reps, " << warmup
		          << " warmup, alternating order"
		          << (cyclePolicy.empty() ? "" : ", aarc-cycle=" + cyclePolicy) << ") ===\n";
		std::printf("%-6s %10s %10s %10s %10s %10s  %9s %9s %9s %11s %11s %8s %8s %10s %11s %10s %8s\n",
		            "gc", "median_ms", "p25", "p75", "min", "max",
		            "allocs", "live", "peakLive", "heapBytes", "reusedBytes",
		            "cycleRcl", "scans", "scanVisited", "cycleBytes", "cycleNs", "errs");

		std::vector<double> medians;
		for (size_t i = 0; i < backends.size(); i++) {
			std::vector<double> times;
			for (const Sample& s : samples[i]) times.push_back(s.ms);
			std::sort(times.begin(), times.end());
			medians.push_back(percentile(times, 0.5));

			const Sample& last = samples[i].back();
			bool truncated = false;
			for (const Sample& s : samples[i]) truncated = truncated || s.truncated;

			std::printf("%-6s %10.2f %10.2f %10.2f %10.2f %10.2f  %9llu %9llu %9llu %11llu %11llu %8llu %8llu %10llu %11llu %10llu %8llu%s\n",
			            backends[i].name,
			            percentile(times, 0.5), percentile(times, 0.25),
			            percentile(times, 0.75), times.front(), times.back(),
			            static_cast<unsigned long long>(last.gc.allocations),
			            static_cast<unsigned long long>(last.heap.liveObjects),
			            static_cast<unsigned long long>(last.heap.peakLiveObjects),
			            static_cast<unsigned long long>(last.heap.bytesCommitted),
			            static_cast<unsigned long long>(last.heap.bytesReused),
			            static_cast<unsigned long long>(last.gc.cycleObjectsReclaimed),
			            static_cast<unsigned long long>(last.gc.cycleScans),
			            static_cast<unsigned long long>(last.gc.cycleScanVisited),
			            static_cast<unsigned long long>(last.gc.cycleReclaimedBytes),
			            static_cast<unsigned long long>(last.gc.cycleCollectNs),
			            static_cast<unsigned long long>(last.gc.invalidHandleOps),
			            truncated ? "  TRUNCATED" : "");
		}

		// Every backend must have produced identical program output, or the
		// timing comparison is comparing different work.
		bool sameOutput = true;
		for (size_t i = 1; i < samples.size(); i++) {
			if (!samples[i].empty() && !samples[0].empty() &&
			    samples[i].back().output != samples[0].back().output) {
				sameOutput = false;
			}
		}
		std::printf("  program output identical across backends: %s\n",
		            sameOutput ? "yes" : "NO — results are not comparable");

		// Pairwise ratios against the first backend, medians only.
		for (size_t i = 1; i < backends.size(); i++) {
			if (medians[0] <= 0.0) continue;
			std::printf("  %s/%s median: %.3fx   (%s %.2fms vs %s %.2fms)\n",
			            backends[i].name, backends[0].name, medians[i] / medians[0],
			            backends[i].name, medians[i], backends[0].name, medians[0]);
		}
	}

	return 0;
}
