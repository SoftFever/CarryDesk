#!/usr/bin/env bash
# Configure and build Sunshine on Linux.
#
# Usage: ./build_lnx.sh [release|relwithdebinfo|debug|package]   (default: release)
#
# Each build type gets its own directory under build/ (build/Release, build/RelWithDebInfo,
# build/Debug), so switching types never forces a full rebuild of another. Build dependencies are
# expected to be installed already; see scripts/linux_build.sh for the per-distro package lists.
# CUDA is enabled when CMake finds nvcc and skipped otherwise, and the docs are built only when
# doxygen is installed.
#
# "package" makes a Release build for /usr in build/Package and wraps it in a pacman package
# named sunshine-local, written to build/ and built from the working tree as it is. The package
# provides and conflicts with sunshine, so installing it replaces the distribution's package and
# system updates do not bring that one back. Its dependencies and install hooks come from
# packaging/linux/Arch, and its version is HEAD's commit time in UTC, in the same
# year.monthday.time form as upstream releases.

set -euo pipefail

usage() {
  echo "Usage: $0 [release|relwithdebinfo|debug|package]  (default: release)"
}

if (($# > 1)); then
  usage >&2
  exit 1
fi

arg="${1:-release}"
package=false
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
  package)
    build_type="Release"
    package=true
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
build_root="${root_dir}/build"
if [[ "${package}" == true ]]; then
  build_dir="${build_root}/Package"
else
  build_dir="${build_root}/${build_type}"
fi

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

if [[ "${package}" == true ]]; then
  # A local build is versioned 0.0.0-<commit>, which pacman does not accept, so the package build
  # takes a release-style version. CMake reads these variables the way it does in upstream CI.
  IFS=. read -r year month_day time < <(TZ=UTC0 git -C "${root_dir}" log -1 --format=%cd --date=format-local:%Y.%m%d.%H%M%S)
  BRANCH="$(git -C "${root_dir}" rev-parse --abbrev-ref HEAD)"
  BUILD_VERSION="${year}.$((10#${month_day})).$((10#${time}))"
  COMMIT="$(git -C "${root_dir}" rev-parse HEAD)"
  export BRANCH BUILD_VERSION COMMIT

  cmake_args+=(
    -DCMAKE_INSTALL_PREFIX=/usr
    -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/sunshine
    -DSUNSHINE_ASSETS_DIR=share/sunshine
    -DSUNSHINE_CONFIGURE_PKGBUILD=ON
    -DBUILD_TESTS=OFF
    -DBUILD_DOCS=OFF
  )
elif command -v doxygen > /dev/null; then
  cmake_args+=(-DBUILD_DOCS=ON)
else
  cmake_args+=(-DBUILD_DOCS=OFF)
fi

# ccache is the one CCACHE names, as a command or a path, or else the one on PATH. Launchers the
# caller sets through CMake's CMAKE_<LANG>_COMPILER_LAUNCHER environment variables take precedence.
# CUDA's launcher goes through the environment because CMake reads it only once it finds nvcc, so a
# build without CUDA gets no warning about an unused variable.
if ccache="$(command -v "${CCACHE:-ccache}")"; then
  for lang in C CXX; do
    launcher_var="CMAKE_${lang}_COMPILER_LAUNCHER"
    cmake_args+=("-D${launcher_var}=${!launcher_var:-${ccache}}")
  done
  export CMAKE_CUDA_COMPILER_LAUNCHER="${CMAKE_CUDA_COMPILER_LAUNCHER:-${ccache}}"
elif [[ -n "${CCACHE:-}" ]]; then
  echo "CCACHE is set to ${CCACHE}, which is not an executable; building without ccache" >&2
fi

echo "Configuring ${build_type} build in ${build_dir}"
cmake "${cmake_args[@]}"

echo "Building ${build_type}"
cmake --build "${build_dir}"

if [[ "${package}" != true ]]; then
  echo "Built ${build_dir}/sunshine"
  exit 0
fi

# The configured upstream PKGBUILD clones and builds the sources itself. The definitions appended
# to it keep its metadata but package the tree built above, under a name of its own.
package_dir="${build_dir}/pacman"
mkdir -p "${package_dir}"
cp "${build_dir}/sunshine.install" "${package_dir}/"
{
  cat "${build_dir}/PKGBUILD"
  cat << EOF

pkgname=sunshine-local
provides=("sunshine=\${pkgver}")
conflicts=('sunshine')
makedepends=()
checkdepends=()
source=()
sha256sums=()
unset -f prepare build check

package() {
  DESTDIR="\${pkgdir}" cmake --install $(printf '%q' "${build_dir}")
}
EOF
} > "${package_dir}/PKGBUILD"

# PKGDEST from the environment takes precedence over makepkg.conf, so the package lands in build/
# whatever the user's makepkg configuration says.
export PKGDEST="${build_root}"

echo "Packaging sunshine-local ${BUILD_VERSION}"
(cd "${package_dir}" && makepkg --force --clean)

mapfile -t packages < <(cd "${package_dir}" && makepkg --packagelist)
echo "Built ${packages[*]}"
echo "Install it with: sudo pacman -U ${packages[*]}"
