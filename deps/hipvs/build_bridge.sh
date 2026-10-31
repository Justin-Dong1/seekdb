#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
OUT=${1:?Usage: build_bridge.sh OUTPUT_SO}
CXX=${CXX:-$ROCM_ROOT/bin/hipcc}
test -f "$HIPVS_ROOT/include/cuvs/neighbors/hnsw.h"
test -x "$CXX"
mkdir -p "$(dirname "$OUT")"
"$CXX" -std=c++17 -O2 -fPIC -shared -D__HIP_PLATFORM_AMD__ \
  -I"$HIPVS_ROOT/include" -I"$ROCM_ROOT/include" "$HERE/hipvs_hnsw_bridge.cpp" \
  -L"$HIPVS_ROOT/lib" -Wl,--no-undefined -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" -lcuvs_c -o "$OUT"
echo "HIPVS_HNSW_BRIDGE_BUILT $OUT"
