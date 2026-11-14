"""Smoke suite: is the device alive and speaking the protocol? (~1 s)"""
import pytest

pytestmark = pytest.mark.smoke

STATUS_KEYS = ["uptime_ms", "samples", "rate_ms", "sensor", "errors", "overflows", "reset"]


def test_boot_banner_reports_firmware_version(fresh_device):
    """HIL-SM-01: after a start the device prints BOOT with fw=1.0.0 and a valid reset reason."""
    boot = fresh_device.boot
    assert boot is not None
    assert boot.fw == "1.0.0"
    assert boot.reset in {"POWERON", "SOFTWARE"}


def test_ping_answers_pong(device):
    """HIL-SM-02: PING -> OK PONG."""
    assert device.command("PING").raw == "OK PONG"


def test_version_matches_boot_banner(device):
    """HIL-SM-03: VERSION reports the same version as the BOOT line."""
    resp = device.expect_ok("VERSION")
    assert resp.raw == "OK VERSION 1.0.0"
    assert resp.args[0] == device.boot.fw


def test_status_has_every_key_in_order(device):
    """HIL-SM-04: STATUS has all seven fields, in the documented order."""
    status = device.status()
    assert list(status) == STATUS_KEYS
    assert status["sensor"] == "OK"
    assert status["rate_ms"] == 1000
