#!/usr/bin/env bash
# Build the C kit for the D binding. DUB runs this before compiling, so
# `dub build`, `dub run` and `dub test` work from a clean checkout.
#
# Set BUI_SOURCE_DIR to build another BUI checkout.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
package="$(cd "$here/.." && pwd)"
root="${BUI_SOURCE_DIR:-$(cd "$package/../.." && pwd)}"

if [ ! -f "$root/kit/include/BUI_kit.h" ]; then
  echo "BUI source tree not found at '$root' (set BUI_SOURCE_DIR)" >&2
  exit 1
fi

build="$package/.bui/build"
lib="$package/.bui/lib"
mkdir -p "$build" "$lib"

if [ ! -f "$build/CMakeCache.txt" ]; then
  cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DBUI_BUILD_TESTS=OFF \
    -DBUI_BUILD_EXAMPLES=OFF \
    -DBUI_BUILD_PYTHON_BINDINGS=OFF \
    -DBUI_BUILD_LUA_BINDINGS=OFF \
    -DBUI_BUILD_QT_BACKEND=OFF \
    -DBUI_BUILD_IMGUI=OFF
fi

cmake --build "$build" --config Release --target bui_kit bui_api

for name in kit gpu core api; do
  file="libbui_$name.a"
  source="$build/$name/Release/$file"
  if [ ! -f "$source" ]; then
    source="$build/$name/$file"
  fi
  if [ ! -f "$source" ]; then
    echo "built library not found: $source" >&2
    exit 1
  fi
  cp -f "$source" "$lib/$file"
done
