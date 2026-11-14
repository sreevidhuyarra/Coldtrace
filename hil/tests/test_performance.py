"""Performance suite: command round-trip latency."""
import json
import statistics
from pathlib import Path

import pytest

pytestmark = pytest.mark.perf

REPORTS = Path(__file__).resolve().parents[2] / "reports"
P95_LIMIT_MS = {"sim": 50.0}     # only the simulator has a hard limit


def test_ping_latency(device, target, record_property):
    """HIL-PF-01: PING round trip over 200 calls; p50 / p95 / max reported,
    p95 must be under 50 ms on the simulator."""
    latencies = sorted(device.command("PING").latency_ms for _ in range(200))
    result = {
        "target": target.name,
        "calls": len(latencies),
        "p50_ms": round(statistics.median(latencies), 2),
        "p95_ms": round(latencies[int(len(latencies) * 0.95) - 1], 2),
        "max_ms": round(latencies[-1], 2),
    }
    REPORTS.mkdir(exist_ok=True)
    (REPORTS / f"latency-{target.name}.json").write_text(json.dumps(result, indent=2) + "\n")
    for key, value in result.items():
        record_property(key, value)
    print(f"PING latency on {target.name}: {result}")

    limit = P95_LIMIT_MS.get(target.name)
    if limit is not None:
        assert result["p95_ms"] < limit, result
