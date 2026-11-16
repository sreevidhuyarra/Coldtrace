"""ColdTrace Azure Function: IoT Hub telemetry -> Table Storage.

Triggered by IoT Hub's built-in, Event Hub-compatible endpoint (its own
consumer group, so `az iot hub monitor-events` can watch alongside). The logic
lives in pipeline.py; this file only adapts Azure's types to it.

App settings:
  IOTHUB_EVENTS          Event Hub-compatible connection string of the hub (with EntityPath)
  IOTHUB_EVENTHUB_NAME   Event Hub-compatible name
  IOTHUB_CONSUMER_GROUP  consumer group for this function (coldtrace-fn)
  COLDTRACE_STORAGE      storage account connection string for the tables

The Functions Python worker checks the trigger parameter's annotation, and
only accepts typing.List[func.EventHubEvent] for a batch: the built-in
list[...] generic fails with "invalid non-type annotation" and the app loads
0 functions. For the same reason there is no `from __future__ import annotations`.
"""
import json
import logging
import os
from datetime import datetime, timezone
from typing import List

import azure.functions as func

from pipeline import BadMessage, parse, process
from table_store import TableStore

app = func.FunctionApp()
_store: TableStore | None = None


def store() -> TableStore:
    global _store
    if _store is None:                      # one client per worker, tables created once
        _store = TableStore(os.environ["COLDTRACE_STORAGE"])
    return _store


def _utc(t: datetime | None) -> datetime:
    if t is None:
        return datetime.now(timezone.utc)
    return t if t.tzinfo else t.replace(tzinfo=timezone.utc)


def _device(event: func.EventHubEvent, body: bytes) -> str:
    """The device id IoT Hub authenticated, so a device can't write as another.
    Falls back to the payload only if the hub's metadata is missing."""
    meta = event.iothub_metadata or {}
    dev = meta.get("connection-device-id")
    if dev:
        return dev
    try:
        return str(json.loads(body).get("dev", ""))
    except (ValueError, AttributeError):
        return ""


@app.event_hub_message_trigger(
    arg_name="events",
    event_hub_name="%IOTHUB_EVENTHUB_NAME%",
    connection="IOTHUB_EVENTS",
    consumer_group="%IOTHUB_CONSUMER_GROUP%",
    cardinality=func.Cardinality.MANY,
)
def ingest(events: List[func.EventHubEvent]) -> None:
    readings, rejected = [], 0
    for e in events:
        body = e.get_body()
        try:
            readings.append(parse(body, _device(e, body), _utc(e.enqueued_time)))
        except BadMessage as err:
            rejected += 1
            logging.warning("rejected message: %s: %r", err, body[:200])
    out = process(readings, store(), datetime.now(timezone.utc))
    logging.info("batch of %d: stored %d, duplicates %d, rejected %d, alerts raised %s, cleared %s",
                 len(events), out.stored, out.duplicates, rejected, out.alerts_raised, out.alerts_cleared)
