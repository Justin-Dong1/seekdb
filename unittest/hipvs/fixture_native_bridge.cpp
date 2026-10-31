/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Functional SQL-route fixture: builds ONLY ON CPU. Never GPU evidence.
#include "src/oblib/lib/vector/hipvs_hnsw_bridge.h"
#include <vsag/vsag.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

template <typename T> T read(const std::string &bytes, size_t offset) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error("truncated CPU fixture serialization");
  }
  T result;
  std::memcpy(&result, bytes.data() + offset, sizeof(T));
  return result;
}
extern "C" uint32_t seekdb_hipvs_hnsw_abi_version() { return 1; }
extern "C" void seekdb_hipvs_hnsw_free(void *bytes) { std::free(bytes); }
extern "C" int seekdb_hipvs_hnsw_build(const float *vectors, uint64_t rows, uint64_t dim,
    uint32_t degree, uint32_t ef, void **bytes, uint64_t *size, char *error, uint64_t capacity)
{
  *bytes = nullptr;
  *size = 0;
  try {
    const std::string parameters = "{\"dtype\":\"float32\",\"metric_type\":\"l2\",\"dim\":" +
        std::to_string(dim) + ",\"use_old_serial_format\":true,\"hnsw\":{\"use_conjugate_graph\":false,"
        "\"max_degree\":" + std::to_string(degree) + ",\"ef_construction\":" + std::to_string(ef) + "}}";
    auto made = vsag::Factory::CreateIndex("hnsw", parameters);
    if (!made) { throw std::runtime_error(made.error().message); }
    std::vector<int64_t> ids(rows);
    std::iota(ids.begin(), ids.end(), INT64_C(0));
    auto data = vsag::Dataset::Make()->Dim(dim)->NumElements(rows)->Ids(ids.data())
        ->Float32Vectors(vectors)->Owner(false);
    auto built = made.value()->Build(data);
    if (!built || !built.value().empty()) { throw std::runtime_error("CPU fixture Build failed"); }
    std::ostringstream stream(std::ios::binary);
    if (!made.value()->Serialize(stream)) { throw std::runtime_error("CPU fixture Serialize failed"); }
    const std::string wire = stream.str();
    const uint64_t stride = read<uint64_t>(wire, 24);
    if (rows != read<uint64_t>(wire, 16) || stride > wire.size() / rows) {
      throw std::runtime_error("unexpected CPU fixture HNSW layout");
    }
    size_t end = 96 + rows * stride;
    for (uint64_t i = 0; i < rows; ++i) {
      const uint32_t upper = read<uint32_t>(wire, end);
      if (end + 4 > wire.size() || upper > wire.size() - end - 4) {
        throw std::runtime_error("invalid CPU fixture HNSW upper layer");
      }
      end += 4 + upper;
    }
    // Exclude VSAG's own footer, retaining the standard HNSW graph and hierarchy.
    *bytes = std::malloc(end);
    if (!*bytes) { throw std::bad_alloc(); }
    std::memcpy(*bytes, wire.data(), end);
    *size = end;
    std::fprintf(stderr, "CPU_FIXTURE_NATIVE_BRIDGE rows=%llu dim=%llu GPU_EXECUTED=0\n",
        static_cast<unsigned long long>(rows), static_cast<unsigned long long>(dim));
    std::fflush(stderr);
    return 0;
  } catch (const std::exception &failure) {
    if (capacity) { std::snprintf(error, capacity, "%s", failure.what()); }
    return 1;
  }
}
