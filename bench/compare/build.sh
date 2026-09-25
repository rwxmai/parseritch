#!/usr/bin/env bash
# Build the cross-library runners.  PEERS=<dir with the cloned competitors> ./build.sh
# parseritch is x86-only (x86_64 under Rosetta on Apple Silicon); every peer is
# built twice: x86_64 (-march=x86-64-v2, same as parseritch's default) and
# native arm64, so peers can be compared both like-for-like and at full speed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
PEERS="${PEERS:?set PEERS to the directory holding the cloned peer repos}"
out="$here/bin"; mkdir -p "$out"
CXX="${CXX:-clang++}"
X86=(-arch x86_64 -march=x86-64-v2 -mprefer-vector-width=128)
ARM=(-arch arm64)

# parseritch: default (x86-64-v2) and AVX2 libraries from the project's CMake build.
common=(-std=c++20 -O3 -DNDEBUG -DITCH_LEVEL_LINEAR_CHUNKS=4 -I"$root/include")
$CXX "${common[@]}" "${X86[@]}" "$here/run_parseritch.cpp" "$root/build/libitch.a" -o "$out/parseritch.x86"
$CXX "${common[@]}" -arch x86_64 -march=x86-64-v3 "$here/run_parseritch.cpp" "$root/build/libitch_avx2.a" -o "$out/parseritch_avx2.x86"

# itchcpp: libitch.a from its own CMake Release build (build-x86 / build-arm).
ic="$PEERS/bbalouki_itchcpp"
$CXX -std=c++20 -O3 -DNDEBUG "${X86[@]}" -I"$ic/include" -I"$ic" "$here/run_itchcpp.cpp" "$ic/build-x86/src/libitch.a" -o "$out/itchcpp.x86"
$CXX -std=c++20 -O3 -DNDEBUG "${ARM[@]}" -I"$ic/include" -I"$ic" "$here/run_itchcpp.cpp" "$ic/build-arm/src/libitch.a" -o "$out/itchcpp.arm"

# charles-cooper/itch-order-book: header-only apart from bufferedreader.cpp (replaced in the runner).
cc="$PEERS/charles-cooper_itch-order-book"
$CXX -std=c++17 -O3 -DNDEBUG -w "${X86[@]}" -I"$cc" "$here/run_ccooper.cpp" -o "$out/ccooper.x86"
$CXX -std=c++17 -O3 -DNDEBUG -w "${ARM[@]}" -I"$cc" "$here/run_ccooper.cpp" -o "$out/ccooper.arm"

ls -la "$out"

# CppTrader: static libs from its own CMake Release build (build-x86 / build-arm).
ct="$PEERS/chronoxor_CppTrader"
for a in x86 arm; do
  if [ $a = x86 ]; then fl=("${X86[@]}"); else fl=("${ARM[@]}"); fi
  b="$ct/build-$a"
  $CXX -std=c++20 -O3 -DNDEBUG -w "${fl[@]}" -I"$ct/include" -I"$ct/modules" -I"$ct/modules/CppCommon/include" \
      -I"$ct/modules/CppCommon/modules/fmt/include" \
      "$here/run_cpptrader.cpp" "$b/libcpptrader.a" "$b/modules/CppCommon/libcppcommon.a" \
      "$b/modules/CppCommon/modules/libfmt.a" -framework CoreFoundation -o "$out/cpptrader.$a"
done
