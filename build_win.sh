#!/usr/bin/env bash
# Configure and build Sunshine on Windows with MSYS2.
#
# Usage: ./build_win.sh [release|relwithdebinfo|debug|package]   (default: release)
#
# Each build type gets its own directory under build/ (build/Release, build/RelWithDebInfo,
# build/Debug), so switching types never forces a full rebuild of another. The build runs in an
# MSYS2 UCRT64 shell (CLANGARM64 on ARM64), and the script starts one itself when run from anywhere
# else, such as Git Bash; from PowerShell or cmd, run build_win.bat with the same arguments. MSYS2
# is expected in C:\msys64 (set MSYS2_ROOT otherwise) with the packages from docs/building.md
# installed. The docs are built only when doxygen is installed.
#
# The web UI needs native Windows Node.js, which is not on the PATH of an MSYS2 shell. The script
# hands CMake the npm beside the node found on the PATH it starts with; when starting from an
# MSYS2 terminal, set NPM to the npm.cmd or npm.exe of a native Node.js install instead.
#
# "package" makes a Release build in build/Package and wraps it in an NSIS installer and a portable
# zip, written to build/ as Sunshine-Windows-<arch>-installer.exe and
# Sunshine-Windows-<arch>-lite.zip. Their version is HEAD's commit time in UTC, in the same
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

# bash.exe started straight from Windows, as from PowerShell, has only the Windows PATH, which
# lacks the MSYS2 tools used before switching shells.
if [[ -z "${MSYSTEM:-}" ]]; then
  PATH="/usr/bin:${PATH}"
fi

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Look for native Node.js before switching shells, since an MSYS2 login shell drops the Windows
# PATH.
if [[ -z "${NPM:-}" ]] && node="$(command -v node)"; then
  node_dir="$(dirname "${node}")"
  for candidate in npm.cmd npm.exe; do
    if [[ -f "${node_dir}/${candidate}" ]]; then
      NPM="$(cygpath -m "${node_dir}/${candidate}")"
      break
    fi
  done
fi

case "${MSYSTEM:-}" in
  UCRT64 | CLANGARM64) ;;
  *)
    msys2_shell="${MSYS2_ROOT:-/c/msys64}/msys2_shell.cmd"
    if [[ ! -f "${msys2_shell}" ]]; then
      echo "MSYS2 not found at ${MSYS2_ROOT:-/c/msys64}; set MSYS2_ROOT to its install directory." >&2
      exit 1
    fi
    if [[ "${PROCESSOR_ARCHITECTURE:-}" == ARM64 || "${PROCESSOR_ARCHITEW6432:-}" == ARM64 ]]; then
      environment=-clangarm64
    else
      environment=-ucrt64
    fi
    # msys2_shell.cmd starts the shell through cmd.exe, which hands it the whole environment. An
    # MSYS2 bash.exe started straight from Git Bash, which has its own MSYS runtime, receives only
    # a handful of variables and loses NPM, PROCESSOR_ARCHITECTURE and SystemRoot.
    export NPM="${NPM:-}"
    exec "${msys2_shell}" -defterm -here -no-start "${environment}" \
      "${root_dir}/$(basename "${BASH_SOURCE[0]}")" "$@"
    ;;
esac

# CMake's version detection runs git in the current directory, so work from the repository root
# wherever the script was started from.
cd "${root_dir}"

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
  # Packages are made with NSIS and ZIP only. Hiding dotnet skips the WiX setup, which installs its
  # tools with the .NET SDK and fails configure when only the .NET runtime is installed.
  -DDOTNET_EXECUTABLE=OFF
)

if [[ -n "${NPM:-}" ]]; then
  cmake_args+=(-DNPM="${NPM}")
fi

if [[ "${package}" == true ]]; then
  # A local build is versioned 0.0.0, so the package build takes a release-style version instead.
  # CMake reads these variables the way it does in upstream CI.
  IFS=. read -r year month_day time < <(TZ=UTC0 git -C "${root_dir}" log -1 --format=%cd --date=format-local:%Y.%m%d.%H%M%S)
  BRANCH="$(git -C "${root_dir}" rev-parse --abbrev-ref HEAD)"
  BUILD_VERSION="${year}.$((10#${month_day})).$((10#${time}))"
  COMMIT="$(git -C "${root_dir}" rev-parse HEAD)"
  export BRANCH BUILD_VERSION COMMIT

  cmake_args+=(
    -DBUILD_TESTS=OFF
    -DBUILD_DOCS=OFF
  )
elif command -v doxygen > /dev/null; then
  cmake_args+=(-DBUILD_DOCS=ON)
else
  cmake_args+=(-DBUILD_DOCS=OFF)
fi

# ccache is the one CCACHE names, as a command or a Windows or MSYS2 path, or else the one on PATH.
# Launchers the caller sets through CMake's CMAKE_<LANG>_COMPILER_LAUNCHER environment variables
# take precedence. The launcher is an absolute Windows path so builds started from IDEs or shells
# without the MSYS2 PATH still find it.
if ccache="$(command -v "$(cygpath -u "${CCACHE:-ccache}")")"; then
  ccache="$(cygpath -m "${ccache}")"
  for lang in C CXX; do
    launcher_var="CMAKE_${lang}_COMPILER_LAUNCHER"
    cmake_args+=("-D${launcher_var}=${!launcher_var:-${ccache}}")
  done
elif [[ -n "${CCACHE:-}" ]]; then
  echo "CCACHE is set to ${CCACHE}, which is not an executable; building without ccache" >&2
fi

echo "Configuring ${build_type} build in ${build_dir}"
cmake "${cmake_args[@]}"

echo "Building ${build_type}"
cmake --build "${build_dir}"

if [[ "${package}" != true ]]; then
  echo "Built ${build_dir}/sunshine.exe"
  exit 0
fi

echo "Packaging Sunshine ${BUILD_VERSION}"
for generator in NSIS ZIP; do
  cpack -G "${generator}" --config "${build_dir}/CPackConfig.cmake"
done

# The packages go to build/ under the names upstream CI gives its release artifacts.
if [[ "${MSYSTEM}" == CLANGARM64 ]]; then
  arch=ARM64
else
  arch=AMD64
fi
installer="${build_root}/Sunshine-Windows-${arch}-installer.exe"
portable="${build_root}/Sunshine-Windows-${arch}-lite.zip"
mv -f "${build_dir}/cpack_artifacts/Sunshine.exe" "${installer}"
mv -f "${build_dir}/cpack_artifacts/Sunshine.zip" "${portable}"

echo "Built ${installer} and ${portable}"
