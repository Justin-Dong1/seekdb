/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "../../src/oblib/lib/vector/hipvs_hnsw_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(cuvsError_t code, const char *operation) {
  if (code != CUVS_SUCCESS) {
    const char *error = cuvsGetLastErrorText();
    throw std::runtime_error(std::string(operation) + ": " + (error ? error : "hipVS returned error"));
  }
}
struct Resources {
  cuvsResources_t res = 0;
  cuvsCagraIndexParams_t cp = nullptr;
  cuvsCagraIndex_t cagra = nullptr;
  cuvsHnswIndexParams_t hp = nullptr;
  cuvsHnswIndex_t hnsw = nullptr;
  ~Resources() {
    if (hnsw) { cuvsHnswIndexDestroy(hnsw); }
    if (hp) { cuvsHnswIndexParamsDestroy(hp); }
    if (cagra) { cuvsCagraIndexDestroy(cagra); }
    if (cp) { cuvsCagraIndexParamsDestroy(cp); }
    if (res) { cuvsResourcesDestroy(res); }
  }
};
struct File {
  char path[64] = "/tmp/seekdb-hipvs-hnsw-XXXXXX";
  int fd = -1;
  File() { fd = mkstemp(path); if (fd < 0) { throw std::runtime_error("hipVS mkstemp failed"); } }
  ~File() { if (fd >= 0) { close(fd); unlink(path); } }
};
}

extern "C" uint32_t seekdb_hipvs_hnsw_abi_version() { return 1; }
extern "C" void seekdb_hipvs_hnsw_free(void *bytes) { std::free(bytes); }
extern "C" int seekdb_hipvs_hnsw_build(const float *vectors, uint64_t rows,
    uint64_t dimension, uint32_t degree, uint32_t ef_construction,
    void **bytes, uint64_t *size, char *error, uint64_t error_capacity)
{
  if (bytes) { *bytes = nullptr; }
  if (size) { *size = 0; }
  if (error && error_capacity) { error[0] = 0; }
  try {
    if (!vectors || !bytes || !size || rows < 256 || rows > UINT32_MAX ||
        !dimension || dimension > INT32_MAX || degree < 2 || degree > 1024 ||
        rows > SIZE_MAX / sizeof(float) / dimension ||
        rows <= 4UL * degree || ef_construction < degree || ef_construction > INT32_MAX) {
      throw std::invalid_argument("unsupported hipVS HNSW build shape/degree");
    }
    const auto started = std::chrono::steady_clock::now();
    Resources r;
    check(cuvsResourcesCreate(&r.res), "cuvsResourcesCreate");
    check(cuvsCagraIndexParamsCreate(&r.cp), "cuvsCagraIndexParamsCreate");
    r.cp->metric = L2Expanded;
    r.cp->graph_degree = 2 * degree;
    r.cp->intermediate_graph_degree = 4 * degree;
    check(cuvsCagraIndexCreate(&r.cagra), "cuvsCagraIndexCreate");
    int64_t shape[2] = {static_cast<int64_t>(rows), static_cast<int64_t>(dimension)};
    DLManagedTensor dataset{};
    dataset.dl_tensor.data = const_cast<float *>(vectors);
    dataset.dl_tensor.device = {kDLCPU, 0};
    dataset.dl_tensor.ndim = 2;
    dataset.dl_tensor.dtype = {kDLFloat, 32, 1};
    dataset.dl_tensor.shape = shape;
    std::fprintf(stderr, "SEEKDB_HIPVS phase=gpu_build_begin rows=%llu dim=%llu degree=%u api=cuvsCagraBuild\n",
        static_cast<unsigned long long>(rows), static_cast<unsigned long long>(dimension), degree);
    std::fflush(stderr);
    check(cuvsCagraBuild(r.res, r.cp, &dataset, r.cagra), "cuvsCagraBuild");
    check(cuvsStreamSync(r.res), "cuvsStreamSync after CAGRA");
    std::fprintf(stderr, "SEEKDB_HIPVS phase=gpu_build_complete synchronized=1\n");
    check(cuvsHnswIndexParamsCreate(&r.hp), "cuvsHnswIndexParamsCreate");
    r.hp->hierarchy = CPU; // NONE is not a standard, mutable HNSW artifact.
    r.hp->ef_construction = static_cast<int>(ef_construction);
    r.hp->num_threads = 1;
    check(cuvsHnswIndexCreate(&r.hnsw), "cuvsHnswIndexCreate");
    check(cuvsHnswFromCagra(r.res, r.hp, r.cagra, r.hnsw), "cuvsHnswFromCagra hierarchy=CPU");
    File file;
    check(cuvsHnswSerialize(r.res, file.path, r.hnsw), "cuvsHnswSerialize");
    check(cuvsStreamSync(r.res), "cuvsStreamSync after conversion");
    const int saved_fd = open(file.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (saved_fd < 0) { throw std::runtime_error("cannot reopen hipVS HNSW serialization"); }
    close(file.fd);
    file.fd = saved_fd;
    struct stat st;
    if (fstat(file.fd, &st) || st.st_size <= 0 ||
        static_cast<uint64_t>(st.st_size) > SIZE_MAX || lseek(file.fd, 0, SEEK_SET) < 0) {
      throw std::runtime_error("invalid hipVS HNSW serialized file");
    }
    std::unique_ptr<void, decltype(&std::free)> memory(std::malloc(static_cast<size_t>(st.st_size)), std::free);
    if (!memory) { throw std::bad_alloc(); }
    uint64_t copied = 0;
    while (copied < static_cast<uint64_t>(st.st_size)) {
      const ssize_t n = read(file.fd, static_cast<char *>(memory.get()) + copied,
                             static_cast<size_t>(st.st_size) - copied);
      if (n <= 0) { throw std::runtime_error("incomplete hipVS HNSW serialized file"); }
      copied += static_cast<uint64_t>(n);
    }
    *size = copied;
    *bytes = memory.release();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "SEEKDB_HIPVS phase=conversion_complete hierarchy=CPU bytes=%llu seconds=%.9f cpu_fallback=0\n",
        static_cast<unsigned long long>(copied), elapsed);
    std::fflush(stderr);
    return 0;
  } catch (const std::exception &failure) {
    if (error && error_capacity) { std::snprintf(error, static_cast<size_t>(error_capacity), "%s", failure.what()); }
    std::fprintf(stderr, "SEEKDB_HIPVS phase=build_failed cpu_fallback=0 error=%s\n", failure.what());
    std::fflush(stderr);
    return 1;
  } catch (...) {
    if (error && error_capacity) { std::snprintf(error, static_cast<size_t>(error_capacity), "unknown hipVS failure"); }
    return 2;
  }
}
