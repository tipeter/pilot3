#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Generate a self-signed TLS certificate for the ESP32-S3 HTTPS server.
#
# Usage:  chmod +x cert_gen.sh && ./cert_gen.sh
#
# Output: main/certs/server.crt  (PEM certificate – embedded in firmware)
#         main/certs/server.key  (PEM private key  – embedded in firmware)
#
# The certificate is valid for 10 years with RSA-2048 and SHA-256.
# For production, replace with a CA-signed certificate.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

CERT_DIR="$(dirname "$0")/main/certs"
CERT_FILE="$CERT_DIR/server.crt"
KEY_FILE="$CERT_DIR/server.key"
DAYS=3650

mkdir -p "$CERT_DIR"

openssl req -x509 \
    -newkey rsa:2048 \
    -keyout "$KEY_FILE" \
    -out    "$CERT_FILE" \
    -days   "$DAYS" \
    -nodes \
    -subj   "/C=HU/ST=Pilot/L=Pilot/O=ESP32S3 Pilot/CN=esp32s3.local" \
    -addext "subjectAltName=IP:192.168.4.1,DNS:esp32s3.local"

echo ""
echo "✓ Certificate: $CERT_FILE"
echo "✓ Private key: $KEY_FILE"
echo ""
echo "SHA-256 fingerprint:"
openssl x509 -in "$CERT_FILE" -noout -fingerprint -sha256
echo ""
echo "Valid until:"
openssl x509 -in "$CERT_FILE" -noout -enddate
echo ""
echo "Add the certificate to your browser/OS trust store to avoid warnings."
