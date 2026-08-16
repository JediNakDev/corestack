#!/bin/bash
#
# generate_keys.sh
# ----------------
# Generates an RSA-1024 private key and a Certificate Signing Request (CSR).
# This is the C/OpenSSL equivalent of generate_keys.py.
#
# Usage: ./generate_keys.sh [prefix]
#   e.g. ./generate_keys.sh server
#   produces: server_private_key.pem, server_certificate_request.csr
#
# The generated CSR uses these subject fields:
#   C=SG, ST=Singapore, L=Singapore, O=SUTD, CN=sutd.edu.sg

SUFFIX="${1:-}"

KEYFILE="${SUFFIX}_private_key.pem"
CSRFILE="${SUFFIX}_certificate_request.csr"

if [ -z "$SUFFIX" ]; then
    KEYFILE="private_key.pem"
    CSRFILE="certificate_request.csr"
fi

echo "Generating 1024-bit RSA private key..."
openssl genrsa -out "$KEYFILE" 1024

echo "Generating CSR..."
openssl req -new -key "$KEYFILE" -out "$CSRFILE" \
    -subj "/C=SG/ST=Singapore/L=Singapore/O=SUTD/CN=sutd.edu.sg"

echo "Done."
echo "  Private key: $KEYFILE"
echo "  CSR:         $CSRFILE"
