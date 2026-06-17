#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-"$script_dir/build-vs"}"

if [[ -d "$build_dir" ]]; then
    rm -rf -- "$build_dir"
    echo "Removed: $build_dir"
else
    echo "Nothing to clean: $build_dir"
fi
