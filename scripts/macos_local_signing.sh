#!/usr/bin/env bash
# Create the code-signing identity that "./build_mac.sh package" signs local builds with.
#
# macOS ties privacy permissions such as Screen Recording and Accessibility to an app's code
# signature. An ad-hoc signature only identifies one exact build, so every rebuild of an ad-hoc
# signed Sunshine.app loses its permissions, while builds signed with the same certificate keep them.
#
# The identity is a self-signed certificate named "CarryDesk Local Signing" in a keychain of its
# own, so that builds run over SSH can unlock it without the login password. The keychain's
# password is generated here and kept in a file only this user can read. Run this once per Mac; it
# changes nothing when the identity already exists.

set -euo pipefail

identity_name="CarryDesk Local Signing"
keychain="${HOME}/Library/Keychains/carrydesk-local-signing.keychain-db"
password_file="${HOME}/.config/carrydesk/local-signing-keychain.password"

if [[ -f "${keychain}" && -f "${password_file}" ]] &&
  security find-certificate -c "${identity_name}" "${keychain}" > /dev/null 2>&1; then
  echo "${identity_name} already exists in ${keychain}"
  exit 0
fi

if [[ -e "${keychain}" ]]; then
  echo "${keychain} exists but lacks the identity or its password file; delete it with" >&2
  echo "  security delete-keychain \"${keychain}\"" >&2
  echo "and run this script again" >&2
  exit 1
fi

umask 077
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

mkdir -p "$(dirname "${password_file}")"
/usr/bin/openssl rand -base64 32 > "${password_file}"
keychain_password="$(cat "${password_file}")"

security create-keychain -p "${keychain_password}" "${keychain}"
# Without automatic locking the keychain stays unlocked until logout; build_mac.sh unlocks it after that.
security set-keychain-settings "${keychain}"

cat > "${work_dir}/openssl.cnf" << EOF
[req]
distinguished_name = subject
prompt = no
x509_extensions = code_signing

[subject]
CN = ${identity_name}

[code_signing]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
EOF

# /usr/bin/openssl is LibreSSL, whose PKCS#12 encryption the security tool can import.
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -config "${work_dir}/openssl.cnf" \
  -keyout "${work_dir}/key.pem" -out "${work_dir}/cert.pem" 2> /dev/null
p12_password="$(/usr/bin/openssl rand -hex 16)"
/usr/bin/openssl pkcs12 -export -name "${identity_name}" -inkey "${work_dir}/key.pem" -in "${work_dir}/cert.pem" \
  -out "${work_dir}/identity.p12" -passout "pass:${p12_password}"

security import "${work_dir}/identity.p12" -k "${keychain}" -P "${p12_password}" -T /usr/bin/codesign > /dev/null
# Let codesign use the private key without a confirmation dialog, which an SSH session cannot show.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "${keychain_password}" "${keychain}" > /dev/null

# Sign a throwaway binary to prove the identity works. The certificate is not trusted as a root, so
# codesign is given its hash, which it accepts for an untrusted identity.
identity_hash="$(security find-certificate -c "${identity_name}" -Z "${keychain}" | awk '/^SHA-1 hash:/ { print $3 }')"
cp /usr/bin/true "${work_dir}/probe"
codesign --force --sign "${identity_hash}" --keychain "${keychain}" "${work_dir}/probe"
codesign --verify "${work_dir}/probe"

echo "Created ${identity_name} (${identity_hash}) in ${keychain}"
echo "\"./build_mac.sh package\" now signs Sunshine.app with it. Grant its permissions once more after"
echo "installing the next build; later builds keep them."
