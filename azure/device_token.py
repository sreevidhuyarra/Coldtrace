"""Write firmware/src/secrets.h (git-ignored) with a device SAS token.

    .venv/Scripts/python azure/device_token.py [--hub coldtrace-hub-2db5] [--device coldtrace-01] [--days 30]

The device connects to the hub's main host name (<hub>.azure-devices.net),
whose certificate chain is RSA (DigiCert Global Root G2). The device host name
(<hub>.device.azure-devices.net) uses an ECC chain whose verification is far
slower on the ESP32: in Wokwi it pushed the TLS handshake past Azure's patience
(-0x0050, connection reset). `az iot hub generate-sas-token` only signs for the
device host, so the token is signed here with the device key instead (IoT
Hub's documented HMAC-SHA256 recipe, the same one HIL-CT-06 verifies).
The token itself is never printed.
"""
import argparse, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "hil"))
from embedded_hil.cloud import CloudClient   # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--hub", default="coldtrace-hub-2db5")
ap.add_argument("--device", default="coldtrace-01")
ap.add_argument("--rg", default="coldtrace-rg")
ap.add_argument("--storage", default="coldtracest2db5")
ap.add_argument("--days", type=int, default=30)
a = ap.parse_args()

cloud = CloudClient(a.hub, a.device, a.rg, a.storage)
host = f"{a.hub}.azure-devices.net"
cloud.device_host = host                     # sign for the main host, not the device host
expires = int(time.time()) + a.days * 86400
token = cloud.sign_sas(a.device, cloud.device_key(a.device), expires)

out = REPO / "firmware" / "src" / "secrets.h"
out.write_text(
    f"// Generated {time.strftime('%Y-%m-%d')} by azure/device_token.py ({a.days} days). Git-ignored: never commit.\n"
    "#pragma once\n"
    f'#define HUB_HOST  "{host}"\n'
    f'#define DEVICE_ID "{a.device}"\n'
    f'#define SAS_TOKEN "{token}"\n', encoding="ascii")
print(f"wrote {out}: host {host}, device {a.device}, expires "
      f"{time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime(expires))}")
