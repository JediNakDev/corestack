#!/bin/sh
#
# ephemeral_auth.sh - a throwaway CA and server certificate, for test runs.
#
# auth/server_signed.crt is issued by the course CA, whose private key is not
# in this repo, and it expires. Once it has, every suite that speaks to a real
# daemon dies in the handshake with "certificate has expired" - a failure that
# says nothing about the code and that nobody here can fix by re-signing.
#
# So: when the shipped certificate is no longer valid, mint a self-signed CA
# and a leaf under var/ephemeral-auth and let the harness use those instead
# (load_harness.h reads TETRISH_AUTH_DIR). Both ends of the handshake are ours,
# so a CA we made is exactly as meaningful as the one we were given - it proves
# the chain check works, which is all a test needs it to prove.
#
# Prints the directory to use on stdout, and nothing else. Deliberately: the
# caller does `TETRISH_AUTH_DIR=$(tests/ephemeral_auth.sh)`. When the shipped
# material is still valid it prints auth/ and mints nothing.
#
#   auth/ is never modified. The real material stays whatever the course
#   issued, so re-running against a freshly signed certificate needs no undo.

set -eu

cd "$(cd "$(dirname "$0")/.." && pwd)"

real=auth
out=var/ephemeral-auth

# -checkend 60: also treat "expires within the minute" as expired, so a run
# cannot start valid and fail halfway through.
if openssl x509 -in "$real/server_signed.crt" -noout -checkend 60 >/dev/null 2>&1
then
    echo "$real"
    exit 0
fi

if [ -f "$out/server_signed.crt" ] &&
   openssl x509 -in "$out/server_signed.crt" -noout -checkend 3600 >/dev/null 2>&1
then
    echo "$out" # still good from a previous run
    exit 0
fi

echo "auth/server_signed.crt has expired; minting a throwaway CA in $out" >&2

# The openssl sequence lives in scripts/provision_auth.sh, which `make cert`
# also runs. One copy: two would drift, and the RSA-1024 constraint is exactly
# the kind of detail that gets fixed in one of them and not the other.
rm -rf "$out"
../scripts/provision_auth.sh "$out" >&2

# Not certificate material and not per-checkout - carry the real one across
# rather than inventing a second signing key the tests would have to agree on.
cp "$real/jwt_secret" "$out/jwt_secret"
chmod 600 "$out/jwt_secret"

echo "$out"
