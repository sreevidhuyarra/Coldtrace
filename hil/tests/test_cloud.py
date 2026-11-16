"""ColdTrace end-to-end tests: the esp32_cloud firmware in Wokwi, Azure IoT Hub,
the Azure Function and Table Storage, driven from this PC.

    pytest --target wokwi --cloud -m cloud

Timing rules (see the design doc): latency is measured with cloud clocks
(IoT Hub enqueue -> stored) or the PC's clock at both ends, never the device's.
"""
from __future__ import annotations

import statistics
import time
from datetime import datetime, timedelta, timezone

import pytest

from conftest import net_info

pytestmark = [pytest.mark.cloud, pytest.mark.slow, pytest.mark.hooks]


def send(dev) -> int:
    """SEND: read now, queue for the cloud; returns the reading's seq."""
    resp = dev.expect_ok("SEND")
    return int(resp.args[0].split("=")[1])


def wait_for_ack(dev, seq: int, timeout: float) -> None:
    """Wait for EVT ACK <seq>, consuming earlier ACKs on the way."""
    end = time.monotonic() + timeout * dev.timeout_scale
    while True:
        ev = dev.wait_for_event("ACK", max(1.0, (end - time.monotonic()) / dev.timeout_scale))
        if int(ev.args[0]) >= seq:
            return
        if time.monotonic() > end:
            raise TimeoutError(f"no EVT ACK {seq}")


def pct(vals: list[float], p: float) -> float:
    s = sorted(vals)
    return s[max(0, min(len(s) - 1, -(-len(s) * p // 100) - 1))]


def test_ingestion_latency(cloud_ready, cloud, cloud_results):
    """HIL-CT-01: readings reach the database fast. ingest_ms uses cloud clocks
    only (IoT Hub enqueue -> Function stored); the second figure times a
    SEND on the PC to its row appearing, PC clock at both ends."""
    dev = cloud_ready
    boot = net_info(dev)["boot"]
    warm = send(dev)                                   # wake the Function (scale-to-zero)
    cloud.wait_rows(boot, [warm], 120)

    ingest, host_e2e = [], []
    for _ in range(10):
        t0 = time.perf_counter()
        seq = send(dev)
        row = cloud.wait_rows(boot, [seq], 60)[seq]
        host_e2e.append((time.perf_counter() - t0) * 1000)
        ingest.append(float(row["ingest_ms"]))

    cloud_results["ingest_ms"] = {"n": len(ingest), "p50": round(statistics.median(ingest), 1),
                                  "p95": round(pct(ingest, 95), 1), "max": round(max(ingest), 1),
                                  "clock": "cloud: IoT Hub enqueue -> Function stored"}
    cloud_results["send_to_row_ms"] = {"n": len(host_e2e), "p50": round(statistics.median(host_e2e)),
                                       "p95": round(pct(host_e2e, 95)),
                                       "clock": "PC: SEND command -> row visible (includes Wokwi and 0.25 s polling)"}
    assert statistics.median(ingest) < 1000
    assert pct(ingest, 95) < 5000


def test_outage_loses_nothing(cloud_ready, cloud, cloud_results):
    """HIL-CT-02: Wi-Fi down for 2 minutes (FAULT NET); after reconnecting every
    reading taken during the outage is stored exactly once, no gaps."""
    dev = cloud_ready
    dev.expect_ok("REPORT 5000")
    info = net_info(dev)
    boot, first = info["boot"], info["next_seq"]

    dev.expect_ok("FAULT NET ON")
    dev.wait_for_event("NET_DOWN", 15)
    time.sleep(120)                                    # the outage itself, PC clock
    during = net_info(dev)
    last = during["next_seq"] - 1
    assert during["link"] == "DOWN" and during["queued"] >= 3, during

    t_off = time.perf_counter()
    dev.expect_ok("FAULT NET OFF")
    dev.wait_for_event("NET_UP", 90)
    wait_for_ack(dev, last, 60)
    rows = cloud.wait_rows(boot, range(first, last + 1), 120)
    cloud_results["outage"] = {"outage_s": 120, "readings_queued": last - first + 1,
                               "recovered_s": round(time.perf_counter() - t_off, 1),
                               "dropped": net_info(dev)["dropped"]}

    stored = cloud.boot_rows(boot)
    window = [s for s in stored if first <= s <= last]
    assert sorted(window) == list(range(first, last + 1))     # every seq, once (keyed rows)
    assert net_info(dev)["dropped"] == 0


def test_replayed_readings_are_stored_once(cloud_ready, cloud, cloud_results):
    """HIL-CT-03: with FAULT NOACK the device never sees its ack, so it sends the
    same reading again after the 20 s ack timeout. The cloud keeps one row and
    counts the replay."""
    dev = cloud_ready
    boot = net_info(dev)["boot"]
    resends_before = net_info(dev)["resends"]
    dev.expect_ok("FAULT NOACK ON")
    seq = send(dev)

    end = time.monotonic() + 120 * dev.timeout_scale / 4 + 60      # 20 s device time on a slow sim
    while net_info(dev)["resends"] <= resends_before:
        assert time.monotonic() < end, "device never resent the reading"
        time.sleep(3)
    dev.expect_ok("FAULT NOACK OFF")
    wait_for_ack(dev, seq, 60)

    row = cloud.wait_rows(boot, [seq], 60)[seq]
    end = time.monotonic() + 60
    while int(row.get("duplicates", 0)) < 1 and time.monotonic() < end:
        time.sleep(1)
        row = cloud.boot_rows(boot)[seq]
    cloud_results["replay"] = {"seq": seq, "copies_received": 1 + int(row.get("duplicates", 0)), "rows": 1}
    assert int(row["duplicates"]) >= 1
    assert sum(1 for s in cloud.boot_rows(boot) if s == seq) == 1


def test_twin_round_trip(cloud_ready, cloud, cloud_results):
    """HIL-CT-04: a setting changed in the cloud is applied by the device and
    confirmed back. Times on the PC clock from the hub accepting the change."""
    dev = cloud_ready
    t_set = time.perf_counter()        # before the az call: an upper bound incl. CLI start-up
    version = cloud.twin_update({"report_ms": 15000, "limit_c": 6.5})
    while True:
        ev = dev.wait_for_event("TWIN", 30)
        if int(ev.args[0]) == version:
            break
    t_device = ev.host_time - t_set
    reported = cloud.wait_reported_version(version, 60)
    t_cloud = time.perf_counter() - t_set

    cfg = dev.expect_ok("CFG").raw
    cloud_results["twin_round_trip_s"] = {"device_applied": round(t_device, 2),
                                          "reported_in_cloud": round(t_cloud, 2),
                                          "note": "PC clock from just before the az update call, so both "
                                                  "include az CLI start-up (~2-4 s per call)"}
    assert "report_ms=15000" in cfg and "limit_c=6.50" in cfg
    assert reported["report_ms"] == 15000 and reported["limit_c"] == 6.5 and reported["rejected"] == ""


def test_excursion_raises_exactly_one_alert_then_clears(cloud_ready, cloud, cloud_results):
    """HIL-CT-05: above the limit for longer than the alert window -> exactly one
    alert; back below -> it clears. Window 10 s (device time) for a quick test."""
    dev = cloud_ready
    version = cloud.twin_update({"limit_c": 8, "window_s": 10})
    while int(dev.wait_for_event("TWIN", 30).args[0]) != version:
        pass
    dev.expect_ok("REPORT 1000")
    base = send(dev)                                   # a cold reading closes any older excursion
    wait_for_ack(dev, base, 60)

    start = datetime.now(timezone.utc) - timedelta(seconds=5)
    dev.expect_ok("FAULT TEMP 12")
    alert = cloud.wait_alert(lambda a: a["status"] == "OPEN" and a["temp"] > 8, start, 180)
    t_raised = datetime.now(timezone.utc)
    dev.expect_ok("FAULT TEMP 4")
    cleared = cloud.wait_alert(lambda a: a["RowKey"] == alert["RowKey"] and a["status"] == "CLEARED",
                               start, 120)

    raised_here = cloud.alerts_since(start)
    cloud_results["excursion"] = {"alert": alert["RowKey"], "limit_c": alert["limit_c"],
                                  "window_s": alert["window_s"], "raised_temp": alert["temp"],
                                  "cleared_temp": cleared.get("cleared_temp"),
                                  "seen_raised_after_s": round((t_raised - start).total_seconds(), 1)}
    assert len(raised_here) == 1, [a["RowKey"] for a in raised_here]
    assert alert["window_s"] == 10 and alert["limit_c"] == 8


def test_expired_or_wrong_credentials_are_rejected(cloud, cloud_results):
    """HIL-CT-06: the hub accepts a valid SAS token and refuses a tampered one,
    one signed with the wrong key, and one that has expired. Tokens are signed
    here with explicit expiry times. The PC connects as a second identity
    (coldtrace-sec), so the Wokwi device's own session is not disturbed.

    Observed on 2026-11-17: a token that expired 10 s earlier was still accepted,
    so IoT Hub allows some clock-skew grace; the test uses a token expired 15
    minutes ago and records how a 60 s-old expiry is treated."""
    import base64, os
    sec = "coldtrace-sec"
    cloud.ensure_device(sec)
    key = cloud.device_key(sec)
    now = int(time.time())

    valid = cloud.sign_sas(sec, key, now + 600)
    sig_at = valid.index("sig=") + 4
    tampered = valid[:sig_at] + ("A" if valid[sig_at] != "A" else "B") + valid[sig_at + 1:]
    wrong_key = cloud.sign_sas(sec, base64.b64encode(os.urandom(32)).decode(), now + 600)
    long_expired = cloud.sign_sas(sec, key, now - 15 * 60)
    just_expired = cloud.sign_sas(sec, key, now - 60)

    results = {
        "valid_token": cloud.mqtt_connect(sec, valid),
        "tampered_signature": cloud.mqtt_connect(sec, tampered),
        "wrong_key": cloud.mqtt_connect(sec, wrong_key),
        "expired_15_min_ago": cloud.mqtt_connect(sec, long_expired),
        "expired_60_s_ago (observation)": cloud.mqtt_connect(sec, just_expired),
    }
    cloud_results["security"] = results
    assert results["valid_token"] == "accepted"          # the signing recipe itself is right
    assert results["tampered_signature"] == "Not authorized"
    assert results["wrong_key"] == "Not authorized"
    assert results["expired_15_min_ago"] == "Not authorized"


def test_reboots_reconnect_without_watchdog_resets(cloud_ready, cloud, cloud_results):
    """HIL-CT-07 (bug 005): reboot three times; each time the device must come
    back with reset=SOFTWARE (not WATCHDOG), reconnect to the hub and deliver a
    reading. The loop-gap guard in cloud_ready checks the whole time."""
    dev = cloud_ready
    mark = dev.log_mark()
    timings = []
    for _ in range(3):
        t0 = time.perf_counter()
        boot = dev.restart(timeout=30)
        assert boot.reset == "SOFTWARE", boot
        dev.expect_ok("FAULT TEMP 4")             # a reboot clears the override
        dev.wait_for_event("NET_UP", 90)
        boot_id = net_info(dev)["boot"]
        seq = send(dev)
        wait_for_ack(dev, seq, 60)
        cloud.wait_rows(boot_id, [seq], 60)
        timings.append(round(time.perf_counter() - t0, 1))
    chains = sum(1 for _, d, t in dev.log[mark:] if d == "<-" and t.startswith("EVT RESET_CHAIN"))

    # A real hang must still read as a watchdog reset: the restart flag must not mask it.
    dev.clear_events()
    dev.expect_ok("FAULT HANG")
    hang_boot = dev.wait_for_boot(30)
    cloud_results["reboots"] = {"reboot_to_stored_s": timings, "reset_chain_events": chains,
                                "hang_reset_reason": hang_boot.reset}
    assert hang_boot.reset == "WATCHDOG", hang_boot
    dev.expect_ok("FAULT TEMP 4")
    dev.wait_for_event("NET_UP", 90)
