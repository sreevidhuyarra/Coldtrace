// Copy to secrets.h (git-ignored) and fill in, or let azure/device_token.py write it:
//     .venv/Scripts/python azure/device_token.py --hub <hub> --device coldtrace-01
//
// HUB_HOST is the hub's MAIN host name, "<hub>.azure-devices.net" (RSA certificate chain, DigiCert
// Global Root G2). Its ECC alternative "<hub>.device.azure-devices.net" also works, but verifying ECC
// signatures made the TLS handshake so slow in Wokwi that Azure reset it (-0x0050).
// The token's sr= must name the same host: a token signed for one host name is refused by the other
// with CONNACK 5 (not authorised). `az iot hub generate-sas-token` signs only for the device host name.
#pragma once
#define HUB_HOST  "your-hub.azure-devices.net"
#define DEVICE_ID "coldtrace-01"
#define SAS_TOKEN "SharedAccessSignature sr=your-hub.azure-devices.net%2Fdevices%2Fcoldtrace-01&sig=...&se=..."
