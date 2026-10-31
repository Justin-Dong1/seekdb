/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "src/oblib/lib/vector/hipvs_hnsw_snapshot.h"
#include "src/oblib/lib/vector/hipvs_hnsw_client.h"
#include <iostream>

using namespace oceanbase::common::obvsag::hipvs;
int main(int argc, char **argv) {
  if (argc != 2) { throw std::invalid_argument("pass the CPU native bridge fixture"); }
  setenv("SEEKDB_HIPVS_BUILD_LIBRARY", argv[1], 1);
  const uint64_t rows = 1024, dim = 16;
  std::vector<float> vectors(rows * dim);
  std::vector<int64_t> ids(rows);
  for (uint64_t i = 0; i < rows; ++i) {
    ids[i] = (INT64_C(1) << 40) + 1001 + 7 * i;
    for (uint64_t d = 0; d < dim; ++d) { vectors[i * dim + d] = std::sin(float(i * dim + d)); }
  }
  const std::string parameters = "{\"dtype\":\"float32\",\"metric_type\":\"l2\",\"dim\":16,"
      "\"use_old_serial_format\":true,\"hnsw\":{\"max_degree\":16,\"ef_construction\":200,\"use_conjugate_graph\":false}}";
  HnswSnapshot snapshot(dim, 16, 200, parameters, nullptr, HnswBridgeClient::build);
  snapshot.append(vectors.data(), ids.data(), 400);
  snapshot.append(vectors.data() + 400 * dim, ids.data() + 400, rows - 400);
  auto index = snapshot.materialize();
  auto query = vsag::Dataset::Make()->Dim(dim)->NumElements(1)->Float32Vectors(vectors.data() + 42 * dim)->Owner(false);
  auto found = index->KnnSearch(query, 10, "{\"hnsw\":{\"ef_search\":256}}");
  if (!found || found.value()->GetIds()[0] != ids[42]) {
    throw std::runtime_error("native bridge roundtrip returned a row offset or wrong document ID");
  }
  std::stringstream wire(std::ios::in | std::ios::out | std::ios::binary);
  if (!index->Serialize(wire)) { throw std::runtime_error("native bridge roundtrip Serialize failed"); }
  auto restored = vsag::Factory::CreateIndex("hnsw", parameters);
  if (!restored || !restored.value()->Deserialize(wire)) {
    throw std::runtime_error("native bridge roundtrip Deserialize failed");
  }
  auto loaded = restored.value()->KnnSearch(query, 10, "{\"hnsw\":{\"ef_search\":256}}");
  if (!loaded || loaded.value()->GetIds()[0] != ids[42]) { throw std::runtime_error("reloaded bridge graph lost IDs"); }
  std::cout << "Bridge -> validated upper hierarchy -> native query -> reload PASS (CPU fixture; GPU not executed)\n";
}
