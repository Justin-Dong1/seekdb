/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// CPU-only ABI fixture. It does not build a graph or supply GPU evidence.
#include "src/oblib/lib/vector/hipvs_hnsw_bridge.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace {
std::atomic<int> outstanding{0};
std::atomic<bool> large_stack{false};
}
extern "C" int seekdb_hipvs_fixture_outstanding() { return outstanding.load(); }
extern "C" bool seekdb_hipvs_fixture_large_stack() { return large_stack.load(); }
extern "C" uint32_t seekdb_hipvs_hnsw_abi_version() { return 1; }
extern "C" void seekdb_hipvs_hnsw_free(void *bytes) {
  if (bytes) { --outstanding; std::free(bytes); }
}
extern "C" int seekdb_hipvs_hnsw_build(const float *, uint64_t rows, uint64_t,
    uint32_t, uint32_t, void **bytes, uint64_t *size, char *error, uint64_t capacity)
{
  pthread_attr_t attr;
  void *base = nullptr;
  size_t stack_size = 0;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    pthread_attr_getstack(&attr, &base, &stack_size);
    pthread_attr_destroy(&attr);
  }
  large_stack = stack_size >= 32UL * 1024 * 1024;
  *bytes = std::malloc(4);
  *size = 4;
  if (!*bytes) { return 2; }
  ++outstanding;
  std::memcpy(*bytes, "TEST", 4);
  if (rows == 0) {
    std::snprintf(error, capacity, "injected fixture failure after allocation");
    return 1;
  }
  return 0;
}
