/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "src/oblib/lib/vector/hipvs_hnsw_client.h"
#include <iostream>
#include <string>

using oceanbase::common::obvsag::hipvs::HnswBridgeClient;
void check(bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}
int main(int argc, char **argv) {
  check(argc == 2, "pass the CPU-only ABI fixture library");
  setenv("SEEKDB_HIPVS_BUILD_LIBRARY", "/nonexistent/seekdb-hipvs-fixture.so", 1);
  bool missing_failed = false;
  try { HnswBridgeClient::build(nullptr, 512, 8, 16, 200); }
  catch (const std::runtime_error &error) {
    missing_failed = std::string(error.what()).find("cannot load") != std::string::npos;
  }
  check(missing_failed, "missing bridge did not fail clearly");
  setenv("SEEKDB_HIPVS_BUILD_LIBRARY", argv[1], 1);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  check(library != nullptr, "cannot load ABI fixture");
  auto outstanding = reinterpret_cast<int (*)()>(dlsym(library, "seekdb_hipvs_fixture_outstanding"));
  auto large_stack = reinterpret_cast<bool (*)()>(dlsym(library, "seekdb_hipvs_fixture_large_stack"));
  check(outstanding && large_stack, "missing fixture controls");
  check(HnswBridgeClient::build(nullptr, 512, 8, 16, 200) == std::vector<char>({'T','E','S','T'}),
        "bridge bytes did not survive worker completion");
  check(outstanding() == 0 && large_stack(), "success leaked bytes or used a small thread stack");
  bool build_failed = false;
  try { HnswBridgeClient::build(nullptr, 0, 8, 16, 200); }
  catch (const std::runtime_error &error) {
    build_failed = std::string(error.what()).find("injected fixture failure") != std::string::npos;
  }
  check(build_failed && outstanding() == 0, "failed bridge leaked bytes or hid its error");
  dlclose(library);
  std::cout << "Bridge client: lazy-load failure/retry, 32 MiB worker, result ownership and failure cleanup PASS (CPU fixture)\n";
}
