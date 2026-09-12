cmake_minimum_required(VERSION 3.16)
project(aarc CXX)

# Standalone build tree for the AARC engine.  Not wired into the main Loxis
# build yet: this keeps the experimental GC from touching the compiler/VM.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

# Diagnostic counters.  Off in clean Release throughput measurements.
option(AARC_ENABLE_STATS "collect diagnostic counters (spins/rounds/peakQ)" ON)

# AARC is header-only for now; expose include dirs as an interface library so
# later translation units (worker, cycle detector) can be added the same way.
add_library(aarc INTERFACE)
target_include_directories(aarc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aarc INTERFACE cxx_std_17)
target_link_libraries(aarc INTERFACE Threads::Threads)
target_compile_definitions(aarc INTERFACE
    AARC_ENABLE_STATS=$<IF:$<BOOL:${AARC_ENABLE_STATS}>,1,0>)

# ---- tests ---------------------------------------------------------------
# Each test file is added to this list as its phase lands.
set(AARC_TESTS
    test_core
    test_arc
    test_cycle
    test_stress
    test_adaptive
    test_m3
)
foreach(t ${AARC_TESTS})
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE aarc)
    add_test(NAME ${t} COMMAND ${t})
    if(NOT t STREQUAL "test_stress")
        # M2 hard gates: the engine's authority / regime-invariant asserts are
        # live in the (small) deterministic + adaptive tests.  test_stress runs
        # without them to keep the big-pool soak fast.
        target_compile_definitions(${t} PRIVATE AARC_CHECK_INVARIANTS=1)
    endif()
endforeach()

# Suspect accounting: the lazy stack is a superset, so the live count is what
# the adaptive trigger must read.
add_executable(test_suspects tests/test_suspects.cpp)
target_link_libraries(test_suspects PRIVATE aarc)
target_compile_definitions(test_suspects PRIVATE AARC_TEST_HOOKS=1 AARC_CHECK_INVARIANTS=1)
add_test(NAME test_suspects COMMAND test_suspects)

add_executable(test_concurrency tests/test_concurrency.cpp)
target_link_libraries(test_concurrency PRIVATE aarc)
target_compile_definitions(test_concurrency PRIVATE AARC_TEST_HOOKS=1)
add_test(NAME test_concurrency COMMAND test_concurrency)

# M10a batch transport: staged-batch boundaries + differential vs batch=1.
add_executable(test_batch tests/test_batch.cpp)
target_link_libraries(test_batch PRIVATE aarc)
target_compile_definitions(test_batch PRIVATE AARC_TEST_HOOKS=1 AARC_CHECK_INVARIANTS=1)
add_test(NAME test_batch COMMAND test_batch)

add_executable(test_batch_cons tests/test_batch.cpp)
target_link_libraries(test_batch_cons PRIVATE aarc)
target_compile_definitions(test_batch_cons PRIVATE
    AARC_TEST_HOOKS=1 AARC_CHECK_INVARIANTS=1 AARC_CONSERVATIVE=1)
add_test(NAME test_batch_cons COMMAND test_batch_cons)

# Conservative scheme build: forward-only edges + may-components.  Exercises the
# same core/cycle/adaptive/m3 suites under AARC_CONSERVATIVE (a compile-time
# A/B of the promotion design; exact reverse adjacency is the default).
set(AARC_CONSERVATIVE_TESTS
    test_arc
    test_cycle
    test_adaptive
    test_m3
    test_stress
)
foreach(t ${AARC_CONSERVATIVE_TESTS})
    add_executable(${t}_cons tests/${t}.cpp)
    target_link_libraries(${t}_cons PRIVATE aarc)
    target_compile_definitions(${t}_cons PRIVATE AARC_CONSERVATIVE=1)
    if(NOT t STREQUAL "test_stress")
        target_compile_definitions(${t}_cons PRIVATE AARC_CHECK_INVARIANTS=1)
    endif()
endforeach()

# ---- benchmarks (not registered with ctest) --------------------------------
add_executable(bench_alloc bench/bench_alloc.cpp)
target_link_libraries(bench_alloc PRIVATE aarc)

add_executable(bench_compare bench/bench_compare.cpp)
target_link_libraries(bench_compare PRIVATE aarc)

add_executable(bench_local_fastpath bench/bench_local_fastpath.cpp)
target_link_libraries(bench_local_fastpath PRIVATE aarc)

add_executable(bench_adaptive bench/bench_adaptive.cpp)
target_link_libraries(bench_adaptive PRIVATE aarc)

# M6 evaluation: promotion closure complexity (heap-size and component-size
# sweeps + edge-maintenance cost).  The legacy O(pool) closure variant is the
# same source compiled with AARC_LEGACY_POOL_CLOSURE for a direct A/B.
add_executable(bench_promotion_scaling bench/bench_promotion_scaling.cpp)
target_link_libraries(bench_promotion_scaling PRIVATE aarc)
add_executable(bench_promotion_scaling_legacy bench/bench_promotion_scaling.cpp)
target_link_libraries(bench_promotion_scaling_legacy PRIVATE aarc)
target_compile_definitions(bench_promotion_scaling_legacy PRIVATE
    AARC_LEGACY_POOL_CLOSURE=1)

# Adaptive ablation over the SHARED fraction (0% .. 100%).
add_executable(bench_shared_fraction bench/bench_shared_fraction.cpp)
target_link_libraries(bench_shared_fraction PRIVATE aarc)

# f=0 cost decomposition across workload tiers (alloc / edgeAdd / edgeRemove).
add_executable(bench_local_cost bench/bench_local_cost.cpp)
target_link_libraries(bench_local_cost PRIVATE aarc)

# M10a/M10b: SHARED async transport ablation (batch-size sweep inside one binary).
add_executable(bench_async_transport bench/bench_async_transport.cpp)
target_link_libraries(bench_async_transport PRIVATE aarc)

# M11: producer hot-path attribution (AARC_PROFILE cycle probes; sampled 1/64).
add_executable(bench_producer_cost bench/bench_producer_cost.cpp)
target_link_libraries(bench_producer_cost PRIVATE aarc)
target_compile_definitions(bench_producer_cost PRIVATE AARC_PROFILE=1)

# Conservative-scheme variants of the evaluation benches (AARC_CONSERVATIVE).
add_executable(bench_promotion_scaling_cons bench/bench_promotion_scaling.cpp)
target_link_libraries(bench_promotion_scaling_cons PRIVATE aarc)
target_compile_definitions(bench_promotion_scaling_cons PRIVATE AARC_CONSERVATIVE=1)
add_executable(bench_local_cost_cons bench/bench_local_cost.cpp)
target_link_libraries(bench_local_cost_cons PRIVATE aarc)
target_compile_definitions(bench_local_cost_cons PRIVATE AARC_CONSERVATIVE=1)
add_executable(bench_shared_fraction_cons bench/bench_shared_fraction.cpp)
target_link_libraries(bench_shared_fraction_cons PRIVATE aarc)
target_compile_definitions(bench_shared_fraction_cons PRIVATE AARC_CONSERVATIVE=1)

# One-shot T8 locating diagnostics (see bench_diag.cpp).  bench_diag_i is the
# same battery with traversal counters on.
add_executable(bench_diag bench/bench_diag.cpp)
target_link_libraries(bench_diag PRIVATE aarc)
add_executable(bench_diag_i bench/bench_diag.cpp)
target_link_libraries(bench_diag_i PRIVATE aarc)
target_compile_definitions(bench_diag_i PRIVATE AARC_INSTRUMENT=1)

enable_testing()
