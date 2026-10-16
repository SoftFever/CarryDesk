#!/usr/bin/env bash
# Configure and build Sunshine on Linux.
#
# Usage: ./build_lnx.sh [release|relwithdebinfo|debug]   (default: release)
#
# Each build type gets its own cmake-build-<type> directory, so switching types never forces a
# full rebuild of another. Build dependencies are expected to be installed already; see
# scripts/linux_build.sh for the per-distro package lists. CUDA is enabled when CMake finds nvcc
# and skipped otherwise, and the docs are built only when doxygen is installed.

set -euo pipefail

usage() {
  echo "Usage: $0 [release|relwithdebinfo|debug]  (default: release)"
}

if (($# > 1)); then
  usage >&2
  exit 1
fi

arg="${1:-release}"
case "${arg,,}" in
  release)
    build_type="Release"
    ;;
  relwithdebinfo | relwithdepinfo)
    build_type="RelWithDebInfo"
    ;;
  debug)
    build_type="Debug"
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    echo "Unknown build type: ${arg}" >&2
    usage >&2
    exit 1
    ;;
esac

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${root_dir}/cmake-build-${build_type,,}"

# Only initialize submodules that are missing, so a submodule checked out at another commit for
# local work is left alone.
mapfile -t missing_submodules < <(git -C "${root_dir}" submodule status | awk '/^-/ { print $2 }')
if ((${#missing_submodules[@]} > 0)); then
  echo "Initializing submodules: ${missing_submodules[*]}"
  git -C "${root_dir}" submodule update --init --recursive -- "${missing_submodules[@]}"
fi

cmake_args=(
  -S "${root_dir}"
  -B "${build_dir}"
  -G Ninja
  -DCMAKE_BUILD_TYPE="${build_type}"
  -DCUDA_FAIL_ON_MISSING=OFF
)

if command -v doxygen > /dev/null; then
  cmake_args+=(-DBUILD_DOCS=ON)
else
  cmake_args+=(-DBUILD_DOCS=OFF)
fi

if command -v ccache > /dev/null; then
  cmake_args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

echo "Configuring ${build_type} build in ${build_dir}"
cmake "${cmake_args[@]}"

echo "Building ${build_type}"
cmake --build "${build_dir}"

echo "Built ${build_dir}/sunshine"
