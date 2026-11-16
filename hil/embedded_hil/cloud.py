"""CloudClient: the Azure side of the ColdTrace end-to-end tests.

Drives Azure the way an operator would: the az CLI for the device twin, direct
methods and SAS tokens, and the Table Storage SDK for what the pipeline stored.
Credentials come from the caller's `az login` at run time; nothing secret is
stored in the repository.
"""
from __future__ import annotations

import json
import shutil
import ssl
import subprocess
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Iterable, Optional


def _az_path() -> str:
    found = shutil.which("az") or shutil.which("az.cmd")
    if found:
        return found
    win = Path(r"C:\Program Files\Microsoft SDKs\Azure\CLI2\wbin\az.cmd")
    if win.exists():
        return str(win)
    raise RuntimeError("Azure CLI not found: install it and run `az login`")


def _v(x: Any) -> Any:
    return getattr(x, "value", x)     # Table Storage Int64 columns come back wrapped


class CloudClient:
    def __init__(self, hub: str, device: str, resource_group: str, storage_account: str) -> None:
        self.hub, self.device = hub, device
        self.resource_group, self.storage_account = resource_group, storage_account
        self._az = _az_path()
        from azure.data.tables import TableServiceClient   # imported here: only cloud runs need it
        conn = self.az("storage", "account", "show-connection-string", "-n", storage_account,
                       "-g", resource_group, output="tsv")
        svc = TableServiceClient.from_connection_string(conn)
        self.readings = svc.get_table_client("readings")
        self.alerts_table = svc.get_table_client("alerts")
        self.device_host = self.az("iot", "hub", "show", "-n", hub,
                                   "--query", "properties.deviceHostName", output="tsv")

    # ---- az ----------------------------------------------------------------------------
    def az(self, *args: str, output: str = "json") -> Any:
        r = subprocess.run([self._az, *args, "--only-show-errors", "-o", output],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"az {' '.join(args[:4])}... failed: {r.stderr.strip()}")
        out = r.stdout.strip()
        return json.loads(out) if output == "json" and out else out

    # ---- device twin and methods ---------------------------------------------------------
    def twin_update(self, desired: dict) -> int:
        """Set desired properties; returns the new desired $version."""
        twin = self.az("iot", "hub", "device-twin", "update", "-n", self.hub, "-d", self.device,
                       "--desired", json.dumps(desired))
        return int(twin["properties"]["desired"]["$version"])

    def twin_reported(self) -> dict:
        twin = self.az("iot", "hub", "device-twin", "show", "-n", self.hub, "-d", self.device)
        return twin["properties"]["reported"]

    def wait_reported_version(self, version: int, timeout: float) -> dict:
        end = time.monotonic() + timeout
        while True:
            rep = self.twin_reported()
            if rep.get("applied_version") == version:
                return rep
            if time.monotonic() > end:
                raise TimeoutError(f"reported applied_version {rep.get('applied_version')} != {version}")
            time.sleep(1)

    def invoke(self, method: str, payload: Optional[dict] = None, timeout: int = 60) -> dict:
        return self.az("iot", "hub", "invoke-device-method", "-n", self.hub, "-d", self.device,
                       "--method-name", method, "--method-payload", json.dumps(payload or {}),
                       "--timeout", str(timeout))

    # ---- identities and tokens ------------------------------------------------------------
    def ensure_device(self, device_id: str) -> None:
        ids = self.az("iot", "hub", "device-identity", "list", "-n", self.hub, "--query", "[].deviceId")
        if device_id not in ids:
            self.az("iot", "hub", "device-identity", "create", "-n", self.hub, "-d", device_id)

    def sas_token(self, device_id: str, seconds: int) -> str:
        return self.az("iot", "hub", "generate-sas-token", "-n", self.hub, "-d", device_id,
                       "--du", str(seconds), "--query", "sas", output="tsv")

    def device_key(self, device_id: str) -> str:
        return self.az("iot", "hub", "device-identity", "show", "-n", self.hub, "-d", device_id,
                       "--query", "authentication.symmetricKey.primaryKey", output="tsv")

    def sign_sas(self, device_id: str, key_b64: str, expires_epoch: int) -> str:
        """A device SAS token with any expiry, signed here (IoT Hub's documented
        recipe: HMAC-SHA256 of '<url-encoded resource>\\n<expiry>' with the
        device key). Lets a test hold an already-expired token."""
        import base64, hashlib, hmac
        from urllib.parse import quote
        sr = quote(f"{self.device_host}/devices/{device_id}", safe="")
        sig = base64.b64encode(hmac.new(base64.b64decode(key_b64), f"{sr}\n{expires_epoch}".encode(),
                                        hashlib.sha256).digest()).decode()
        return f"SharedAccessSignature sr={sr}&sig={quote(sig, safe='')}&se={expires_epoch}"

    def mqtt_connect(self, device_id: str, token: str, timeout: float = 20.0) -> str:
        """Connect to the hub as device_id over MQTT/TLS from this PC.
        Returns "accepted" or the broker's refusal reason (e.g. "Not authorized")."""
        import paho.mqtt.client as mqtt
        result: dict[str, Any] = {}
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=device_id, protocol=mqtt.MQTTv311)
        c.username_pw_set(f"{self.device_host}/{device_id}/?api-version=2021-04-12", token)
        c.tls_set(cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        c.on_connect = lambda cl, u, flags, rc, props: result.setdefault("rc", rc)
        c.connect(self.device_host, 8883, 60)
        c.loop_start()
        end = time.monotonic() + timeout
        while "rc" not in result and time.monotonic() < end:
            time.sleep(0.1)
        c.loop_stop()
        c.disconnect()
        rc = result.get("rc")
        if rc is None:
            return "no CONNACK"
        return "accepted" if not rc.is_failure else str(rc)

    # ---- stored data -------------------------------------------------------------------
    def boot_rows(self, boot: str) -> dict[int, dict]:
        """seq -> row for one boot of the device (a range scan in its partition)."""
        rows = self.readings.query_entities(
            "PartitionKey eq @d and RowKey ge @lo and RowKey lt @hi",
            parameters={"d": self.device, "lo": f"{boot}-", "hi": f"{boot}."})
        return {int(_v(r["seq"])): dict(r) for r in rows}

    def wait_rows(self, boot: str, seqs: Iterable[int], timeout: float,
                  poll: float = 0.25) -> dict[int, dict]:
        """Wait until every seq in seqs is stored; returns those rows."""
        want = set(seqs)
        end = time.monotonic() + timeout
        while True:
            rows = self.boot_rows(boot)
            if want <= rows.keys():
                return {s: rows[s] for s in want}
            if time.monotonic() > end:
                raise TimeoutError(f"boot {boot}: missing seqs {sorted(want - rows.keys())[:20]}")
            time.sleep(poll)

    def alerts_since(self, since: datetime) -> list[dict]:
        rows = self.alerts_table.query_entities("PartitionKey eq @d", parameters={"d": self.device})
        return [dict(a) for a in rows if a["raised_at"] >= since]

    def wait_alert(self, pred: Callable[[dict], bool], since: datetime, timeout: float) -> dict:
        end = time.monotonic() + timeout
        while True:
            hits = [a for a in self.alerts_since(since) if pred(a)]
            if hits:
                return hits[0]
            if time.monotonic() > end:
                raise TimeoutError("no matching alert")
            time.sleep(1)
