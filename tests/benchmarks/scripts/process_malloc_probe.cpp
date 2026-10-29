// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Caleb Buahin. All rights reserved.
// macOS-only, opt-in profiling companion. See soa_review_2026-10/README.md.
#include <cstdlib>
#include <cstdint>

namespace {
thread_local std::uint64_t allocations = 0, allocated_bytes = 0;
void record(void* result, std::size_t size) {
    if (result) { ++allocations; allocated_bytes += size; }
}
void* measured_malloc(std::size_t size) {
    void* result = std::malloc(size); record(result, size); return result;
}
void* measured_calloc(std::size_t count, std::size_t size) {
    void* result = std::calloc(count, size); record(result, count * size); return result;
}
void* measured_realloc(void* old, std::size_t size) {
    void* result = std::realloc(old, size); record(result, size); return result;
}
void* measured_aligned_alloc(std::size_t alignment, std::size_t size) {
    void* result = std::aligned_alloc(alignment, size); record(result, size); return result;
}
int measured_posix_memalign(void** result, std::size_t alignment, std::size_t size) {
    const int error = posix_memalign(result, alignment, size);
    if (!error) record(*result, size);
    return error;
}
// dyld applies interposition to callers in other images, so these wrappers'
// calls into libSystem reach the real allocator without dlsym or recursion.
#define INTERPOSE(replacement, original) \
    __attribute__((used)) static const struct { const void* with; const void* replace; } \
    interpose_##original __attribute__((section("__DATA,__interpose"))) = \
    {reinterpret_cast<const void*>(&replacement), reinterpret_cast<const void*>(&original)}
INTERPOSE(measured_malloc, malloc);
INTERPOSE(measured_calloc, calloc);
INTERPOSE(measured_realloc, realloc);
INTERPOSE(measured_aligned_alloc, aligned_alloc);
INTERPOSE(measured_posix_memalign, posix_memalign);
}
extern "C" __attribute__((visibility("default")))
void openswmm_perf_malloc_snapshot(std::uint64_t* calls, std::uint64_t* bytes) {
    *calls = allocations; *bytes = allocated_bytes;
}
