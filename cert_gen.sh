#!/bin/bash
# cert_gen.sh
# Generates a self-signed X.509 certificate with SAN extension for mDNS.

HOSTNAME="pilot3.local"
IP_ADDRESS="192.168.0.215"
DAYS=3650
CERT_DIR="main/certs"

mkdir -p "$CERT_DIR"

echo "Generating RSA 2048 private key..."
openssl genrsa -out "$CERT_DIR/server.key" 2048

echo "Generating X.509 certificate for $HOSTNAME..."

# Temporary OpenSSL configuration to inject SAN (Subject Alternative Name)
cat > san.cnf <<EOF
[req]
distinguished_name = req_distinguished_name
x509_extensions = v3_req
prompt = no

[req_distinguished_name]
C = HU
ST = Gyor-Moson-Sopron
L = Mosonmagyarovar
O = Pilot Firmware
CN = $HOSTNAME

[v3_req]
subjectAltName = @alt_names

[alt_names]
DNS.1 = $HOSTNAME
IP.1 = $IP_ADDRESS
EOF

openssl req -x509 -nodes -days $DAYS -newkey rsa:2048 \
  -keyout "$CERT_DIR/server.key" \
  -out "$CERT_DIR/server.crt" \
  -config san.cnf \
  -extensions v3_req

rm san.cnf
echo "Certificate generated successfully with SAN: $HOSTNAME, $IP_ADDRESS"
