# hipVS-assisted native HNSW snapshot construction

This optional Linux implementation builds a CAGRA graph with hipVS, converts it
to standard HNSW with `hierarchy=CPU`, validates/relabels the serialized graph,
and imports it into the original VSAG HNSW index. Queries, persistence, filters,
and subsequent incremental updates continue through VSAG. This accelerates bulk
snapshot construction; it does not accelerate streaming INSERT graph updates.

Supported initial scope: ordinary HNSW, dense float32, L2, no extra-info columns.
Tiny snapshots (fewer than 256 rows or at most `4*M` rows) use an explicitly
logged CPU fallback. Unsupported index shapes/types retain their original CPU
path. A selected GPU build failure returns an error, without a silent CPU retry.

Build the observer using the normal pinned Linux dependencies, adding
`-DOB_BUILD_HIPVS_HNSW=ON` to the Release CMake configuration. The default is OFF.
The observer needs no ROCm headers at compile time and loads no GPU runtime at
startup. On the GPU host, compile the small bridge against the installed SDK:

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  bash deps/hipvs/build_bridge.sh /workspace/amd-native/libseekdb_hipvs_hnsw.so
```

The SDK must provide the hipVS C API (`libcuvs_c.so`) with CAGRA, HNSW conversion
and serialization enabled. API reference: AMD-Ecosystem/hipVS,
`release/rocmds-26.03`, `cpp/include/cuvs/neighbors/{cagra,hnsw}.h`.
The bridge uses `CUVS_SUCCESS` (1), and synchronizes before publishing output.

Enable GPU snapshot builds for a dedicated observer process:

```bash
export SEEKDB_HIPVS_HNSW_BUILD=1
export SEEKDB_HIPVS_BUILD_LIBRARY=/workspace/amd-native/libseekdb_hipvs_hnsw.so
# launch the candidate observer with its own data directory and SQL/RPC ports
```

Use `SEEKDB_HIPVS_HNSW_BUILD=0` for the CPU baseline with the same binary.
Import raw vectors first, then `CREATE VECTOR INDEX ... WITH(type=hnsw,lib=vsag,
distance=l2,...)` to exercise the snapshot route. Creating the index before
inserting rows primarily exercises the incremental CPU route.

`SEEKDB_HIPVS phase=gpu_build_begin/gpu_build_complete/conversion_complete/`
`native_import_complete` mark the actual native build stages. A measurement
must cover all rows, synchronization, conversion, persistence and the first
native readiness query; it must reject any `cpu_fallback` stage. CPU fixture
tests can emit native-import messages, so an import message alone is not GPU
evidence. Do not substitute a batch-query speedup for construction measurements.

Run portable and actual VSAG tests after preparing Linux dependencies:

```bash
bash unittest/hipvs/run_tests.sh
```

The tests cover malformed graph rejection, permuted/64-bit IDs, native queries,
serialization/reload, add/remove, snapshot batching/failure/retry, and bridge
thread/result ownership. Their graph builders are explicitly CPU fixtures.
Real GPU execution, SQL integration, restarts, quality thresholds and repeated
CPU/GPU performance measurements must be validated separately before submission.
