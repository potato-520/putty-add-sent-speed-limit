#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-"$script_dir/build-vs"}"
cmake_exe="${CMAKE_EXE:-/mnt/c/Program Files/CMake/bin/cmake.exe}"

if [[ ! -x "$cmake_exe" ]]; then
    if command -v cmake.exe >/dev/null 2>&1; then
        cmake_exe="$(command -v cmake.exe)"
    else
        echo "error: Windows CMake not found. Set CMAKE_EXE=/path/to/cmake.exe" >&2
        exit 1
    fi
fi

if ! command -v wslpath >/dev/null 2>&1; then
    echo "error: this script is intended to run under WSL." >&2
    exit 1
fi

src_win="$(wslpath -m "$script_dir")"
build_win="$(wslpath -m "$build_dir")"

targets=("$@")
if [[ ${#targets[@]} -eq 0 ]]; then
    targets=(putty plink)
fi

"$cmake_exe" -S "$src_win" -B "$build_win" -G "Visual Studio 17 2022" -A x64
"$cmake_exe" --build "$build_win" --config Release --target "${targets[@]}"

echo
echo "Built targets: ${targets[*]}"
echo "Output directory: $build_dir/Release"
