"""Robustness suite: malformed input and floods must never wedge the parser."""
import random

import pytest

pytestmark = pytest.mark.robustness


@pytest.mark.parametrize("cmd", ["HELLO", "ping", "P1NG"])
def test_unknown_commands_get_err_unknown(device, cmd):
    """HIL-RB-01: unknown or lower-case commands -> ERR UNKNOWN."""
    assert device.command(cmd).raw == "ERR UNKNOWN"


def test_overlong_line_rejected_then_parser_recovers(device):
    """HIL-RB-02: 64 characters are accepted as a line; 65 -> ERR TOOLONG, and the
    next command works."""
    assert device.command("A" * 64).raw == "ERR UNKNOWN"    # parsed: just not a command
    assert device.command("A" * 65).raw == "ERR TOOLONG"
    assert device.command("PING").raw == "OK PONG"


@pytest.mark.parametrize("ending", [b"\n", b"\r\n", b"\r"], ids=["LF", "CRLF", "CR"])
def test_every_line_ending_gives_exactly_one_response(device, ending):
    """HIL-RB-03: PING ended with LF, CRLF or a bare CR gets exactly one OK PONG."""
    device.send_raw(b"PING" + ending)
    assert device.read_response().raw == "OK PONG"
    extra = [line for line in device.drain(0.3) if line]
    assert extra == [], f"unexpected extra lines {extra}"


def test_binary_garbage_does_not_break_the_parser(device):
    """HIL-RB-04: 2 KB of random bytes (fixed seed) followed by a newline: whatever
    the device answers, it must still answer PING correctly afterwards."""
    rng = random.Random(20261115)
    garbage = bytes(rng.randrange(256) for _ in range(2048))
    device.send_raw(garbage + b"\n")
    leftovers = device.drain(0.5)
    assert all(l.startswith(("ERR", "OK")) or not l for l in leftovers), leftovers
    assert device.command("PING").raw == "OK PONG"


def test_flood_of_100_commands_all_answered_in_order(device):
    """HIL-RB-05: 100 commands sent in one burst are all answered, in order."""
    burst = b"".join(b"PING\n" if i % 2 == 0 else b"VERSION\n" for i in range(100))
    device.send_raw(burst)
    for i in range(100):
        expected = "OK PONG" if i % 2 == 0 else "OK VERSION 2.0.0"
        assert device.read_response(2).raw == expected, f"response {i}"
