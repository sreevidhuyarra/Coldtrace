"""pipeline.Store on Azure Table Storage.

Tables:
  readings  PartitionKey = device, RowKey = <boot>-<seq, 10 digits>
            temp, boot, seq, t_ms, enqueued_at, stored_at, ingest_ms, duplicates
  devices   PartitionKey = "device", RowKey = device; last seen + alert state
  alerts    PartitionKey = device, RowKey = alert id; OPEN -> CLEARED
"""
from __future__ import annotations

from datetime import datetime

from azure.core.exceptions import ResourceExistsError, ResourceNotFoundError
from azure.data.tables import EdmType, EntityProperty, TableServiceClient, UpdateMode

from pipeline import DeviceState, Reading

READINGS, DEVICES, ALERTS = "readings", "devices", "alerts"


def _i64(v: int) -> EntityProperty:
    """t_ms and seq can pass 2^31; Table Storage needs them typed as Int64."""
    return EntityProperty(v, EdmType.INT64)


class TableStore:
    def __init__(self, conn_str: str):
        svc = TableServiceClient.from_connection_string(conn_str)
        for name in (READINGS, DEVICES, ALERTS):
            svc.create_table_if_not_exists(name)
        self.readings = svc.get_table_client(READINGS)
        self.devices = svc.get_table_client(DEVICES)
        self.alerts = svc.get_table_client(ALERTS)

    # ---- readings -----------------------------------------------------------------
    def insert_reading(self, r: Reading, stored_at: datetime) -> bool:
        entity = {
            "PartitionKey": r.device, "RowKey": r.row_key,
            "boot": r.boot, "seq": _i64(r.seq), "t_ms": _i64(r.t_ms), "temp": float(r.temp),
            "enqueued_at": r.enqueued_at, "stored_at": stored_at,
            # cloud clocks only: hub receive time -> Function store time
            "ingest_ms": float((stored_at - r.enqueued_at).total_seconds() * 1000.0),
            "duplicates": 0,
        }
        try:
            self.readings.create_entity(entity)    # fails if (device, boot, seq) exists
            return True
        except ResourceExistsError:
            row = self.readings.get_entity(r.device, r.row_key)
            self.readings.update_entity(
                {"PartitionKey": r.device, "RowKey": r.row_key,
                 "duplicates": int(row.get("duplicates", 0)) + 1, "last_duplicate_at": stored_at},
                mode=UpdateMode.MERGE)
            return False

    # ---- per-device state ----------------------------------------------------------
    def get_state(self, device: str) -> DeviceState:
        try:
            e = self.devices.get_entity("device", device)
        except ResourceNotFoundError:
            return DeviceState()
        since = e.get("above_since_ms", -1)
        since = since.value if isinstance(since, EntityProperty) else since
        return DeviceState(
            boot=e.get("boot", ""),
            last_seq=int(getattr(e.get("last_seq", 0), "value", e.get("last_seq", 0))),
            above_since_ms=None if since is None or since < 0 else int(since),
            open_alert=e.get("open_alert") or None,
            last_seen=e.get("last_seen"),
            last_temp=e.get("last_temp"),
        )

    def put_state(self, device: str, s: DeviceState) -> None:
        entity = {
            "PartitionKey": "device", "RowKey": device,
            "boot": s.boot, "last_seq": _i64(s.last_seq),
            "above_since_ms": _i64(-1 if s.above_since_ms is None else s.above_since_ms),
            "open_alert": s.open_alert or "",
        }
        if s.last_seen is not None:
            entity["last_seen"] = s.last_seen
        if s.last_temp is not None:
            entity["last_temp"] = float(s.last_temp)
        self.devices.upsert_entity(entity, mode=UpdateMode.REPLACE)

    # ---- alerts ------------------------------------------------------------------------
    def raise_alert(self, alert_id: str, r: Reading, above_since_ms: int, raised_at: datetime) -> None:
        self.alerts.upsert_entity({
            "PartitionKey": r.device, "RowKey": alert_id, "status": "OPEN",
            "raised_at": raised_at, "raised_seq": _i64(r.seq), "boot": r.boot,
            "temp": float(r.temp), "limit_c": float(r.limit_c), "window_s": int(r.window_s),
            "above_since_ms": _i64(above_since_ms),
        })

    def clear_alert(self, device: str, alert_id: str, r: Reading, cleared_at: datetime) -> None:
        self.alerts.update_entity(
            {"PartitionKey": device, "RowKey": alert_id, "status": "CLEARED",
             "cleared_at": cleared_at, "cleared_seq": _i64(r.seq), "cleared_temp": float(r.temp)},
            mode=UpdateMode.MERGE)
