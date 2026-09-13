#!/bin/sh
#
# provision_auth.sh - mint a local CA and server certificate into an auth dir.
#
#   usage: provision_auth.sh <dir> [--force]
#
# WHY THIS EXISTS
#
# The server key and its signed certificate are no longer in git, and they
# cannot be: a private key in a public repository is a private key you have
# given away. That leaves a fresh clone with no way to complete a handshake,
# because the certificate that used to be committed was signed by a course CA
# whose key nobody here has. Regenerating the real one is not an option.
#
# So this mints a complete local set instead - our own CA, and a leaf signed by
# it. Both ends of the handshake are ours, so a CA we made proves exactly what
# the course CA proved: that the chain check works. Run `make cert` and the
# daemon comes up.
#
# It also covers the other way this breaks. The committed certificate EXPIRES,
# and once it has, every suite that talks to a real daemon dies in the
# handshake with "certificate has expired" - a failure that says nothing about
# the code and that nobody here can fix by re-signing.
#
# RSA-1024, which is not a typo and not a default. libtetrissh checks the
# wrapped session key against a compile-time RSA_KEY_BYTES of 128, so a
# 2048-bit key is refused as a protocol error - and from the client side that
# looks like a handshake that succeeded and a server that then vanished. Match
# the protocol or spend an hour finding that out again.
#
# Refuses to overwrite an existing key unless --force, because the one case
# that must never happen is this script quietly replacing real course-issued
# material minutes before a demo.

set -eu

dir=${1:-}
force=${2:-}

if [ -z "$dir" ]; then
    echo "usage: provision_auth.sh <dir> [--force]" >&2
    exit 2
fi

if [ -f "$dir/private_key.pem" ] && [ "$force" != "--force" ]; then
    exit 0 # already provisioned; say nothing, this runs from `make`
fi

mkdir -p "$dir"

# Same subjects the course material used, so a cert from here is a drop-in for
# one from there and nothing downstream has to care which it got.
subject_ca="/C=SG/ST=SG/L=SG/O=SUTD/OU=ISTD/CN=50005-local"
subject_leaf="/C=SG/ST=Singapore/L=Singapore/O=SUTD/CN=sutd.edu.sg"

echo "provisioning a local CA and server certificate in $dir"

openssl req -x509 -newkey rsa:1024 -nodes -days 3650 \
    -keyout "$dir/ca_key.pem" -out "$dir/ca_local.crt" \
    -subj "$subject_ca" >/dev/null 2>&1

openssl req -new -newkey rsa:1024 -nodes \
    -keyout "$dir/private_key.pem" -out "$dir/certificate_request.csr" \
    -subj "$subject_leaf" >/dev/null 2>&1

openssl x509 -req -in "$dir/certificate_request.csr" \
    -CA "$dir/ca_local.crt" -CAkey "$dir/ca_key.pem" -CAcreateserial \
    -days 3650 -sha256 -out "$dir/server_signed.crt" >/dev/null 2>&1

# The client verifies against cacsertificate.crt by name, in both projects and
# in every fixture. Writing the local CA there is what makes the new leaf
# verify - but the course CA is a tracked file worth keeping, so it is moved
# aside rather than destroyed.
if [ -f "$dir/cacsertificate.crt" ] && [ ! -f "$dir/cacsertificate.course.crt" ]
then
    cp "$dir/cacsertificate.crt" "$dir/cacsertificate.course.crt"
    echo "  kept the course CA as cacsertificate.course.crt"
fi
cp "$dir/ca_local.crt" "$dir/cacsertificate.crt"

chmod 600 "$dir/private_key.pem" "$dir/ca_key.pem"

echo "  done. This is a LOCAL chain - it verifies against itself, not against"
echo "  anything the course issued. Restore cacsertificate.course.crt and drop"
echo "  in real material when you need to talk to someone else's server."
