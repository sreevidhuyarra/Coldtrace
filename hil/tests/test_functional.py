"""Functional suite: every command does what docs/protocol.md says.

Timing is checked with the device's own timestamps (t_ms in sample lines), so
results do not depend on how fast the host or the simulator runs.
"""
import pytest

pytestmark = pytest.mark.functional


def stream_samples(device, rate_ms: int, count: int, timeout: float):
    """Stream at rate_ms until `count` sample events have arrived."""
    device.expect_ok(f"RATE {rate_ms}")
    device.expect_ok("STREAM ON")
    events = [device.wait_for_event("S", timeout) for _ in range(count)]
    device.expect_ok("STREAM OFF")
    return [int(e.args[0]) for e in events]


def test_read_matches_sensor_setting(device, target):
    """HIL-FN-01: READ is within +/-0.1 C of the temperature the sensor is set to."""
    assert device.read_temp() == pytest.approx(target.expected_temp, abs=0.1)


@pytest.mark.parametrize("rate, expected", [
    ("99", "ERR RANGE"),
    ("100", "OK RATE 100"),
    ("60000", "OK RATE 60000"),
    ("60001", "ERR RANGE"),
])
def test_rate_boundaries(device, rate, expected):
    """HIL-FN-02: RATE accepts 100..60000 ms and rejects values just outside."""
    assert device.command(f"RATE {rate}").raw == expected


@pytest.mark.parametrize("cmd", ["RATE", "RATE abc", "RATE -5", "RATE 1 2"])
def test_rate_rejects_bad_arguments(device, cmd):
    """HIL-FN-03: RATE without a single unsigned number -> ERR ARGS, rate unchanged."""
    assert device.command(cmd).raw == "ERR ARGS"
    assert device.status()["rate_ms"] == 1000


def test_samples_are_spaced_at_rate(device):
    """HIL-FN-04: at RATE 200, consecutive samples are 200 ms apart (+/-20%)."""
    stamps = stream_samples(device, 200, 8, timeout=2)
    gaps = [b - a for a, b in zip(stamps, stamps[1:])]
    assert all(160 <= g <= 240 for g in gaps), f"gaps {gaps}"


def test_dump_returns_oldest_first_with_rising_timestamps(device):
    """HIL-FN-05: DUMP lists every stored sample, oldest first."""
    stream_samples(device, 100, 5, timeout=2)
    samples = device.dump()
    assert len(samples) >= 5
    stamps = [s.t_ms for s in samples]
    assert stamps == sorted(stamps) and len(set(stamps)) == len(stamps)


def test_dump_n_returns_the_newest_n(device):
    """HIL-FN-06: DUMP 3 returns exactly the 3 newest samples."""
    stream_samples(device, 100, 5, timeout=2)
    device.expect_ok("RATE 60000")              # freeze the buffer
    everything = device.dump()
    assert device.dump(3) == everything[-3:]


def test_clear_empties_the_buffer(device):
    """HIL-FN-07: CLEAR leaves DUMP with nothing to return."""
    stream_samples(device, 100, 3, timeout=2)
    device.expect_ok("RATE 60000")              # no new sample can sneak in
    assert device.command("CLEAR").raw == "OK CLEAR"
    assert device.dump() == []
    assert device.status()["samples"] == 0


def test_stream_on_emits_samples_and_off_stops_them(device, target):
    """HIL-FN-08: STREAM ON emits EVT S per sample; after STREAM OFF none arrive."""
    device.expect_ok("RATE 100")
    assert device.command("STREAM ON").raw == "OK STREAM ON"
    event = device.wait_for_event("S", 2)
    assert float(event.args[1]) == pytest.approx(target.expected_temp, abs=0.1)
    assert device.command("STREAM OFF").raw == "OK STREAM OFF"
    device.clear_events()
    assert device.collect_events("S", 0.6) == []


def events_read_mid_command(log) -> int:
    """How many EVT S lines were read while a command was waiting for its response
    (between a sent line and the OK/ERR that answered it)."""
    count, in_flight = 0, False
    for _, direction, text in log:
        if direction == "->":
            in_flight = True
        elif text.startswith(("OK", "ERR")):
            in_flight = False
        elif in_flight and text.startswith("EVT S "):
            count += 1
    return count


def test_commands_work_while_streaming(device):
    """HIL-FN-09: with samples streaming every 100 ms, keep sending commands until 5
    sample events have been read while a command was waiting for its response.
    Every command must still get its own correct response; no EVT line may be
    mistaken for one."""
    mark = device.log_mark()
    device.expect_ok("RATE 100")
    device.expect_ok("STREAM ON")
    for _ in range(2000):                     # guard against an endless loop
        if events_read_mid_command(device.log[mark:]) >= 5:
            break
        assert device.command("PING").raw == "OK PONG"
        assert device.status()["rate_ms"] == 100
    else:
        pytest.fail("sample events never arrived during a command")
    device.expect_ok("STREAM OFF")
