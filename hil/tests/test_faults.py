"""Fault suite: an unplugged sensor, simulated with the FAULT I2C test hook.

The firmware must keep serving commands, report the loss once, and recover on
its own within the 5 s retry window when the sensor comes back.
"""
import pytest

from embedded_hil import DeviceClient, ProcessTransport

pytestmark = pytest.mark.fault


def lose_sensor(device):
    device.expect_ok("FAULT I2C ON")
    assert device.command("READ").raw == "ERR SENSOR"
    return device.wait_for_event("SENSOR_LOST", 5)


@pytest.mark.hooks
def test_i2c_fault_gives_err_sensor_and_sensor_lost(device):
    """HIL-FT-01: with the bus failing, READ -> ERR SENSOR and EVT SENSOR_LOST within 5 s."""
    lose_sensor(device)
    assert device.status()["sensor"] == "LOST"


@pytest.mark.hooks
def test_error_counter_rises_on_failed_transactions(device):
    """HIL-FT-02: every failed I2C transaction increments STATUS errors."""
    before = device.status()["errors"]
    device.expect_ok("FAULT I2C ON")
    device.command("READ")
    device.command("READ")
    assert device.status()["errors"] >= before + 2


@pytest.mark.hooks
@pytest.mark.slow
def test_sensor_recovers_when_fault_cleared(device, target):
    """HIL-FT-03: FAULT I2C OFF -> EVT SENSOR_OK within 7 s, then readings resume."""
    lose_sensor(device)
    assert device.command("FAULT I2C OFF").ok
    device.wait_for_event("SENSOR_OK", 7)
    assert device.read_temp() == pytest.approx(target.expected_temp, abs=0.1)


@pytest.mark.hooks
@pytest.mark.slow
def test_commands_still_served_while_sensor_lost(device):
    """HIL-FT-04: while LOST the device answers commands normally and reports LOST once."""
    lose_sensor(device)
    for _ in range(5):
        assert device.command("PING").raw == "OK PONG"
    assert device.status()["sensor"] == "LOST"
    assert device.collect_events("SENSOR_LOST", 1.5) == [], "SENSOR_LOST must be reported once"


def test_production_build_rejects_fault_commands(device, target):
    """HIL-FT-05: the production build answers FAULT with ERR UNKNOWN and does not hang."""
    if target.name == "sim":
        transport = ProcessTransport(target.sim_prod_path, target.expected_temp)
        prod = DeviceClient(transport)
        try:
            prod.wait_for_boot(5)
            _assert_no_fault_hooks(prod)
        finally:
            device.log.extend(prod.log)         # show it in this test's report
            transport.close()
    elif not device.has_hooks():
        _assert_no_fault_hooks(device)
    else:
        pytest.skip("this target runs the test-hook build; nothing to check here")


def _assert_no_fault_hooks(dev):
    assert dev.command("FAULT I2C ON").raw == "ERR UNKNOWN"
    assert dev.command("FAULT HANG").raw == "ERR UNKNOWN"
    assert dev.command("PING").raw == "OK PONG"
    assert dev.read_temp()  # sensor untouched
