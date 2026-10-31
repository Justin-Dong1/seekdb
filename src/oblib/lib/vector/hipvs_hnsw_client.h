/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "hipvs_hnsw_bridge.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <vector>

namespace oceanbase { namespace common { namespace obvsag { namespace hipvs {

// Loads only when a GPU build is requested, after normal seekdb initialization.
// In particular, no ROCm objects/threads are created during process startup.
class HnswBridgeClient {
public:
  static std::vector<char> build(const float *vectors, uint64_t rows,
                                  uint64_t dim, uint32_t degree, uint32_t ef)
  {
    const Api &api = get_api();
    Job job{&api, vectors, rows, dim, degree, ef, nullptr, 0, {}, -1};
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) { throw std::runtime_error(std::string("hipVS pthread_attr_init: ") + std::strerror(rc)); }
    rc = pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024);
    pthread_t thread;
    if (rc == 0) { rc = pthread_create(&thread, &attr, run, &job); }
    pthread_attr_destroy(&attr);
    if (rc) { throw std::runtime_error(std::string("hipVS pthread_create: ") + std::strerror(rc)); }
    // A valid, newly created joinable thread has one owner, this call.
    rc = pthread_join(thread, nullptr);
    if (rc) { std::terminate(); } // returning would leave the worker using a dead stack Job
    std::unique_ptr<void, decltype(api.free)> owner(job.bytes, api.free);
    if (job.rc != 0 || !job.bytes || job.size == 0 || job.size > SIZE_MAX) {
      throw std::runtime_error(std::string("hipVS HNSW build failed: ") + job.error);
    }
    const char *begin = static_cast<const char *>(job.bytes);
    return std::vector<char>(begin, begin + static_cast<size_t>(job.size));
  }

private:
  struct Api {
    void *library;
    decltype(&seekdb_hipvs_hnsw_build) build;
    decltype(&seekdb_hipvs_hnsw_free) free;
  };
  struct Job {
    const Api *api;
    const float *vectors;
    uint64_t rows, dim;
    uint32_t degree, ef;
    void *bytes;
    uint64_t size;
    char error[1024];
    int rc;
  };
  static void *run(void *argument)
  {
    Job &job = *static_cast<Job *>(argument);
    job.rc = job.api->build(job.vectors, job.rows, job.dim, job.degree, job.ef,
                             &job.bytes, &job.size, job.error, sizeof(job.error));
    return nullptr;
  }
  template <typename T> static T symbol(void *library, const char *name)
  {
    void *address = dlsym(library, name);
    if (!address) { throw std::runtime_error(std::string("missing hipVS bridge symbol: ") + name); }
    return reinterpret_cast<T>(address);
  }
  static const Api &get_api()
  {
    // Keep a successfully loaded bridge for process lifetime. This avoids
    // unloading ROCm code while another index or thread is using it.
    static const Api api = [] {
      const char *path = std::getenv("SEEKDB_HIPVS_BUILD_LIBRARY");
      void *library = dlopen(path && *path ? path : "libseekdb_hipvs_hnsw.so", RTLD_NOW | RTLD_LOCAL);
      if (!library) {
        const char *message = dlerror();
        throw std::runtime_error(std::string("cannot load hipVS build bridge: ") + (message ? message : "unknown"));
      }
      try {
        auto version = symbol<decltype(&seekdb_hipvs_hnsw_abi_version)>(library, "seekdb_hipvs_hnsw_abi_version");
        if (version() != 1) { throw std::runtime_error("unsupported hipVS HNSW bridge ABI"); }
        return Api{library, symbol<decltype(&seekdb_hipvs_hnsw_build)>(library, "seekdb_hipvs_hnsw_build"),
                   symbol<decltype(&seekdb_hipvs_hnsw_free)>(library, "seekdb_hipvs_hnsw_free")};
      } catch (...) {
        dlclose(library);
        throw;
      }
    }();
    return api;
  }
};

}}}} // namespace oceanbase::common::obvsag::hipvs
