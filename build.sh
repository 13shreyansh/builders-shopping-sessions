#!/bin/sh
set -eu
python="${PYTHON:-python3}"
lib=$("$python" -c 'import pyarrow; print(pyarrow.get_library_dirs()[0])')
inc=$("$python" -c 'import pyarrow; print(pyarrow.get_include())')
case "$(uname -s)" in
  Darwin) arrow="$lib/libarrow.2000.dylib"; parquet="$lib/libparquet.2000.dylib";;
  Linux) arrow="$lib/libarrow.so.2000"; parquet="$lib/libparquet.so.2000";;
esac
"${CXX:-c++}" -std=c++17 -O3 -DNDEBUG -pthread -I"$inc" pipeline.cpp "$parquet" "$arrow" -Wl,-rpath,"$lib" -o "${1:-pipeline}"
