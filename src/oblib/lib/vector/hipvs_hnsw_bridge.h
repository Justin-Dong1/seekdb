/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <cstdint>

// ABI version 1. The bridge owns the returned bytes until bridge_free is called.
// It produces standard hnswlib serialization with hierarchy=CPU and row-offset
// labels. The server validates and replaces those labels before native import.
extern "C" {
uint32_t seekdb_hipvs_hnsw_abi_version();
int seekdb_hipvs_hnsw_build(const float *vectors, uint64_t rows, uint64_t dimension,
                          uint32_t degree, uint32_t ef_construction,
                          void **bytes, uint64_t *size, char *error, uint64_t error_capacity);
void seekdb_hipvs_hnsw_free(void *bytes);
}
