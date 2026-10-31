#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${HIPVS_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/seekdb-hipvs-tests-XXXXXX")}
VSAG_ROOT=${VSAG_ROOT:-$ROOT/deps/3rd/usr/local/oceanbase/deps/devel}
CXX=${CXX:-g++}
mkdir -p "$OUT"
export LD_LIBRARY_PATH="$VSAG_ROOT/lib:$VSAG_ROOT/lib/vsag_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$ROOT" \
  "$ROOT/unittest/hipvs/test_hnsw_import.cpp" -o "$OUT/test_wire"
"$OUT/test_wire"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -fPIC -shared -I"$ROOT" \
  "$ROOT/unittest/hipvs/fixture_bridge.cpp" -pthread -o "$OUT/libfixture_bridge.so"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$ROOT" \
  "$ROOT/unittest/hipvs/test_bridge_client.cpp" -ldl -pthread -o "$OUT/test_bridge_client"
"$OUT/test_bridge_client" "$OUT/libfixture_bridge.so"
test -f "$VSAG_ROOT/include/vsag/vsag.h"
"$CXX" -std=c++17 -O2 -DAMD_TEST_VSAG -I"$ROOT" -I"$VSAG_ROOT/include" \
  "$ROOT/unittest/hipvs/test_hnsw_import.cpp" -L"$VSAG_ROOT/lib/vsag_lib" \
  -Wl,-rpath,"$VSAG_ROOT/lib/vsag_lib" -Wl,-rpath-link,"$VSAG_ROOT/lib" \
  -lvsag -o "$OUT/test_native"
"$OUT/test_native"
"$CXX" -std=c++17 -O2 -fPIC -shared -I"$ROOT" -I"$VSAG_ROOT/include" \
  "$ROOT/unittest/hipvs/fixture_native_bridge.cpp" -L"$VSAG_ROOT/lib/vsag_lib" \
  -Wl,-rpath,"$VSAG_ROOT/lib/vsag_lib" -Wl,-rpath-link,"$VSAG_ROOT/lib" \
  -lvsag -o "$OUT/libfixture_native_bridge.so"
"$CXX" -std=c++17 -O2 -I"$ROOT" -I"$VSAG_ROOT/include" \
  "$ROOT/unittest/hipvs/test_native_bridge_roundtrip.cpp" -L"$VSAG_ROOT/lib/vsag_lib" \
  -Wl,-rpath,"$VSAG_ROOT/lib/vsag_lib" -Wl,-rpath-link,"$VSAG_ROOT/lib" \
  -lvsag -ldl -pthread -o "$OUT/test_native_bridge_roundtrip"
"$OUT/test_native_bridge_roundtrip" "$OUT/libfixture_native_bridge.so"
echo "CPU_FIXTURE_TEST_ARTIFACTS $OUT (not GPU build evidence)"
