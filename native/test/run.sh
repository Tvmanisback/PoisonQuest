#!/usr/bin/env bash
# Host-side check of the loader and native mods against a fake libil2cpp.so (no device needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CXX="${CXX:-c++}"
"$CXX" -std=c++17 -O1 -Wall -Wextra -fPIC -shared -I stubs -I ../src \
    -DPOISON_NAMESPACE='"Poison"' -DPOISON_CLASS='"Plugin"' -DPOISON_METHOD='"Bootstrap"' \
    ../src/poison_loader.cpp ../src/il2cpp_util.cpp ../src/mods.cpp \
    -o "$OUT/libPoison.so" -ldl -pthread
"$CXX" -std=c++17 -fPIC -shared -Wl,-soname,libil2cpp.so fake_il2cpp.cpp -o "$OUT/libil2cpp.so"
"$CXX" -std=c++17 test_loader.cpp -o "$OUT/test_loader" -ldl

LD_LIBRARY_PATH="$OUT" "$OUT/test_loader" "$OUT/libPoison.so" "$OUT/libil2cpp.so" "$OUT/native.cfg"
