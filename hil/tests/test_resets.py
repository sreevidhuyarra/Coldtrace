"""Reset suite: software reset and watchdog reset."""
import pytest

pytestmark = pytest.mark.reset


def test_reset_reboots_with_software_reason_and_defaults(device):
    """HIL-RS-01: RESET -> OK RESET, then BOOT reset=SOFTWARE; settings back to defaults."""
    device.expect_ok("RATE 500")
    device.expect_ok("STREAM ON")
    assert device.command("RESET").raw == "OK RESET"
    boot = device.wait_for_boot(5)
    assert boot.reset == "SOFTWARE"
    device.clear_events()
    assert device.status()["rate_ms"] == 1000
    assert device.collect_events("S", 1.5) == [], "streaming must be off after a reset"


@pytest.mark.hooks
@pytest.mark.slow
def test_hang_triggers_watchdog_reset(device):
    """HIL-RS-02: FAULT HANG stops the watchdog feed -> BOOT reset=WATCHDOG within 6 s."""
    assert device.command("FAULT HANG").raw == "OK FAULT HANG"
    boot = device.wait_for_boot(6)
    assert boot.reset == "WATCHDOG"
    assert device.command("PING").raw == "OK PONG"


def test_commands_queued_behind_reset_are_dropped(device):
    """HIL-RS-03 (bug 003): commands already sent after RESET are lost with the reboot;
    the line after OK RESET is BOOT."""
    mark = device.log_mark()
    device.send_raw(b"RESET\nRATE 500\nPING\n")
    assert device.read_response().raw == "OK RESET"
    assert device.wait_for_boot(5).reset == "SOFTWARE"
    received = [text for _, d, text in device.log[mark:] if d == "<-"]
    assert "OK RATE 500" not in received and "OK PONG" not in received, received
    assert device.status()["rate_ms"] == 1000


@pytest.mark.wokwi_limit(reason="Wokwi does not restart millis() on a software reset (docs/bugs/002)")
def test_uptime_restarts_after_reset(device):
    """HIL-RS-04: uptime_ms starts again from 0 after RESET (real ESP32 behaviour)."""
    device.expect_ok("RESET")
    device.wait_for_boot(5)
    assert device.status()["uptime_ms"] < 2000
