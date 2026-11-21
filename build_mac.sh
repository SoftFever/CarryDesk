#!/usr/bin/env bash
# Configure and build Sunshine on macOS.
#
# Usage: ./build_mac.sh [release|relwithdebinfo|debug|package]   (default: release)
#
# Each build type gets its own directory under build/ (build/Release, build/RelWithDebInfo,
# build/Debug), so switching types never forces a full rebuild of another. Build dependencies are
# expected to be installed through Homebrew already; see scripts/macos_build.sh for the formula
# list, and Python 3.11 or newer is needed as well. The docs are built only when doxygen is
# installed. Tests are left out unless the BUILD_TESTS environment variable turns them on, as in
# "BUILD_TESTS=ON ./build_mac.sh debug"; the package never builds them.
#
# "package" makes a Release build in build/Package and wraps it in a DragNDrop DMG named
# Sunshine-macOS-<arch>, written to build/ and built from the working tree as it is. Its version
# is HEAD's commit time in UTC, in the same year.monthday.time form as upstream releases, and it
# targets the same minimum macOS version as upstream releases. The bundle is signed during
# packaging when APPLE_CODESIGN_IDENTITY names a "Developer ID Application" identity; notarizing
# it for other people is left to xcrun notarytool. Without one, it is signed with the local
# identity that scripts/macos_local_signing.sh creates, which lets rebuilds keep the Screen
# Recording and Accessibility permissions, or ad hoc when that identity does not exist.

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
case "$(printf '%s' "${arg}" | tr '[:upper:]' '[:lower:]')" in
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
missing_submodules=()
while IFS= read -r submodule; do
  missing_submodules+=("${submodule}")
done < <(git -C "${root_dir}" submodule status | awk '/^-/ { print $2 }')
if ((${#missing_submodules[@]} > 0)); then
  echo "Initializing submodules: ${missing_submodules[*]}"
  git -C "${root_dir}" submodule update --init --recursive -- "${missing_submodules[@]}"
fi

cmake_args=(
  -S "${root_dir}"
  -B "${build_dir}"
  -G Ninja
  -DCMAKE_BUILD_TYPE="${build_type}"
  -DBUILD_TESTS="${BUILD_TESTS:-OFF}"
)

if [[ "${package}" == true ]]; then
  # A local build is versioned 0.0.0-<commit>, so the package build takes a release-style version
  # instead. CMake reads these variables the way it does in upstream CI.
  IFS=. read -r year month_day time < <(TZ=UTC0 git -C "${root_dir}" log -1 --format=%cd --date=format-local:%Y.%m%d.%H%M%S)
  BRANCH="$(git -C "${root_dir}" rev-parse --abbrev-ref HEAD)"
  BUILD_VERSION="${year}.$((10#${month_day})).$((10#${time}))"
  COMMIT="$(git -C "${root_dir}" rev-parse HEAD)"
  export BRANCH BUILD_VERSION COMMIT

  # Upstream releases are built for this minimum macOS version; the caller can raise it through
  # the environment.
  export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"

  # cmake/packaging/macos.cmake signs the bundle while cpack installs it, when SHOULD_SIGN is
  # true and CMake was configured with the identity.
  if [[ -n "${APPLE_CODESIGN_IDENTITY:-}" ]]; then
    cmake_args+=(-DAPPLE_CODESIGN_IDENTITY="${APPLE_CODESIGN_IDENTITY}")
    export SHOULD_SIGN=true
  else
    export SHOULD_SIGN=false

    # Otherwise it signs with the identity scripts/macos_local_signing.sh creates, when that
    # exists, so the build keeps the privacy permissions granted to earlier ones.
    local_keychain="${HOME}/Library/Keychains/carrydesk-local-signing.keychain-db"
    local_password_file="${HOME}/.config/carrydesk/local-signing-keychain.password"
    if [[ -f "${local_keychain}" && -f "${local_password_file}" ]]; then
      security unlock-keychain -p "$(cat "${local_password_file}")" "${local_keychain}"
      local_identity="$(security find-certificate -c "CarryDesk Local Signing" -Z "${local_keychain}" |
        awk '/^SHA-1 hash:/ { print $3 }')"
      if [[ -n "${local_identity}" ]]; then
        export LOCAL_CODESIGN_IDENTITY="${local_identity}" LOCAL_CODESIGN_KEYCHAIN="${local_keychain}"
      fi
    fi
  fi

  # A package is configured from scratch, so nothing an earlier configure cached, such as the
  # minimum macOS version or the OpenSSL it found, carries over into it.
  cmake_args+=(
    --fresh
    -DBUILD_TESTS=OFF
    -DBUILD_DOCS=OFF
  )
elif command -v doxygen > /dev/null; then
  cmake_args+=(-DBUILD_DOCS=ON)
else
  cmake_args+=(-DBUILD_DOCS=OFF)
fi

# Homebrew keeps several dependencies keg-only, so hand CMake and the compiler their prefixes.
# Homebrew reports the would-be prefix of a formula that is not installed as well, so each hint is
# added only when its prefix really exists.
if ! command -v brew > /dev/null; then
  echo "Homebrew not found; install the dependencies from scripts/macos_build.sh first" >&2
  exit 1
fi

brew_prefix() {
  local prefix
  prefix="$(brew --prefix "$1" 2> /dev/null)" || return 1
  [[ -d "${prefix}" ]] || return 1
  printf '%s\n' "${prefix}"
}

# Static Boost.Locale exposes ICU as transitive linker arguments, while Homebrew keeps ICU
# keg-only. The ICU has to be the one Homebrew's boost was built against, since that is the one
# its static libraries were compiled for.
icu4c_formula="$(brew deps --formula boost 2> /dev/null | awk '/^icu4c/ { print $1; exit }' || true)"
icu4c_root="$(brew_prefix "${icu4c_formula:-icu4c}" || true)"
if [[ -n "${icu4c_root}" ]]; then
  export CPPFLAGS="${CPPFLAGS:+${CPPFLAGS} }-I${icu4c_root}/include"
  export LDFLAGS="${LDFLAGS:+${LDFLAGS} }-L${icu4c_root}/lib"
  export LIBRARY_PATH="${icu4c_root}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
  cmake_args+=(-DICU_ROOT="${icu4c_root}")
else
  echo "icu4c not found; static Boost.Locale from Homebrew needs it to link" >&2
fi

# Upstream builds against openssl@4, which Homebrew also links into /opt/homebrew/include, a
# directory other dependencies put on the include path. Headers and libraries only match when
# CMake links that same OpenSSL, so openssl@3 is used only when openssl@4 is missing.
for openssl_formula in openssl@4 openssl@3; do
  if openssl_root="$(brew_prefix "${openssl_formula}")"; then
    cmake_args+=(-DOPENSSL_ROOT_DIR="${openssl_root}")
    break
  fi
done
if opus_root="$(brew_prefix opus)"; then
  cmake_args+=(-DOpus_ROOT_DIR="${opus_root}")
fi
if qtbase_root="$(brew_prefix qtbase)"; then
  cmake_args+=(-DQt6_DIR="${qtbase_root}/lib/cmake/Qt6")
fi
if qtsvg_root="$(brew_prefix qtsvg)"; then
  cmake_args+=(-DQt6Svg_DIR="${qtsvg_root}/lib/cmake/Qt6Svg")
fi
# The docs tooling needs Python 3.11 or newer, which the system /usr/bin/python3 is not, and the
# Homebrew one loses to it whenever /usr/bin comes first on the PATH.
if python_root="$(brew_prefix python)" && [[ -x "${python_root}/bin/python3" ]]; then
  cmake_args+=("-DPython3_EXECUTABLE=${python_root}/bin/python3")
fi

# ccache is the one CCACHE names, as a command or a path, or else the one on PATH. Launchers the
# caller sets through CMake's CMAKE_<LANG>_COMPILER_LAUNCHER environment variables take precedence.
if ccache="$(command -v "${CCACHE:-ccache}")"; then
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
  echo "Built ${build_dir}/Sunshine.app"
  exit 0
fi

echo "Packaging Sunshine ${BUILD_VERSION}"

# cpack can rarely fail with "hdiutil: create failed - Resource busy", so allow one retry, the
# same way upstream CI does.
if ! cpack -G DragNDrop --config "${build_dir}/CPackConfig.cmake"; then
  echo "cpack failed, retrying once with verbose output" >&2
  cpack -G DragNDrop --config "${build_dir}/CPackConfig.cmake" --verbose
fi

dmg="${build_root}/Sunshine-macOS-$(uname -m).dmg"
mv -f "${build_dir}/cpack_artifacts/Sunshine.dmg" "${dmg}"
echo "Built ${dmg}"

if [[ "${SHOULD_SIGN}" == true ]]; then
  echo "Notarize it with: xcrun notarytool submit ${dmg} --keychain-profile notarytool-password --wait"
  echo "Then staple it with: xcrun stapler staple ${dmg}"
else
  if [[ -n "${LOCAL_CODESIGN_IDENTITY:-}" ]]; then
    echo "The bundle is signed with the local CarryDesk identity, so it keeps earlier builds' permissions"
  else
    echo "The bundle is signed ad hoc; run scripts/macos_local_signing.sh once so rebuilds keep permissions"
  fi
  echo "After dragging Sunshine.app to /Applications, clear its quarantine with: xattr -cr /Applications/Sunshine.app"
fi
