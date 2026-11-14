"""Fixtures, command-line options and reporting for the HIL suite.

    pytest --target sim                       # host simulator (default)
    pytest --target wokwi                     # Wokwi running in VS Code
    pytest --target serial --port COM3        # a real board

Reports go to reports/<target>-report.html (self-contained) and
reports/<target>-junit.xml unless --html / --junitxml are given.
"""
from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import pytest

from embedded_hil import DeviceClient, DeviceTimeout, ProcessTransport, SerialTransport

REPO = Path(__file__).resolve().parents[2]
EXE = ".exe" if os.name == "nt" else ""
WOKWI_URL = "rfc2217://localhost:4000"
# Wokwi simulates slower than real time (about 40% speed seen on Day 1), so
# every timeout is stretched there. Device-side timestamps are used for timing
# checks, so this only affects how long the harness is willing to wait.
DEFAULT_TIMEOUT_SCALE = {"sim": 1.0, "wokwi": 4.0, "serial": 1.5}

LOG_START = pytest.StashKey[int]()
DEVICE = pytest.StashKey[DeviceClient]()
RUN_INFO = pytest.StashKey[dict]()


@dataclass(frozen=True)
class Target:
    name: str
    port: Optional[str]
    expected_temp: float
    timeout_scale: float
    sim_path: Path
    sim_prod_path: Path


# ---- options --------------------------------------------------------------------

def pytest_addoption(parser: pytest.Parser) -> None:
    g = parser.getgroup("hil", "hardware-in-the-loop target")
    g.addoption("--target", choices=["sim", "wokwi", "serial"], default="sim",
                help="device to test (default: sim)")
    g.addoption("--port", default=None,
                help=f"serial port or pyserial URL (wokwi default: {WOKWI_URL})")
    g.addoption("--expected-temp", type=float, default=24.0,
                help="temperature the sensor is set to, in C (default: 24.0)")
    g.addoption("--sim-path", default=str(REPO / "build" / f"sim{EXE}"),
                help="host simulator binary (default: build/sim)")
    g.addoption("--timeout-scale", type=float, default=None,
                help="multiply every timeout (default: sim 1, wokwi 4, serial 1.5)")


@pytest.hookimpl(tryfirst=True)
def pytest_configure(config: pytest.Config) -> None:
    target = config.getoption("--target")
    reports = REPO / "reports"
    reports.mkdir(exist_ok=True)
    if hasattr(config.option, "htmlpath") and not config.option.htmlpath:
        config.option.htmlpath = str(reports / f"{target}-report.html")
        config.option.self_contained_html = True
    if not getattr(config.option, "xmlpath", None):
        config.option.xmlpath = str(reports / f"{target}-junit.xml")
    config.stash[RUN_INFO] = {"target": target, "fw": "?", "commit": _git_commit()}


def _git_commit() -> str:
    try:
        sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO,
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=REPO,
                               capture_output=True, text=True, check=True).stdout.strip()
        return sha + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    """Known Wokwi limitations become xfail (with their reason) on that target only."""
    if config.getoption("--target") != "wokwi":
        return
    for item in items:
        mark = item.get_closest_marker("wokwi_limit")
        if mark:
            item.add_marker(pytest.mark.xfail(reason=mark.kwargs.get("reason", "Wokwi limitation"),
                                              strict=True))


def pytest_report_header(config: pytest.Config) -> str:
    info = config.stash[RUN_INFO]
    return f"HIL target: {info['target']}   commit: {info['commit']}"


# ---- fixtures ---------------------------------------------------------------------

@pytest.fixture(scope="session")
def target(pytestconfig: pytest.Config) -> Target:
    name = pytestconfig.getoption("--target")
    scale = pytestconfig.getoption("--timeout-scale") or DEFAULT_TIMEOUT_SCALE[name]
    sim_path = Path(pytestconfig.getoption("--sim-path"))
    return Target(
        name=name,
        port=pytestconfig.getoption("--port"),
        expected_temp=pytestconfig.getoption("--expected-temp"),
        timeout_scale=scale,
        sim_path=sim_path,
        sim_prod_path=sim_path.with_name(f"sim_prod{EXE}"),
    )


@pytest.fixture(scope="session")
def device(target: Target, pytestconfig: pytest.Config):
    """One connection for the whole run; tests start from a clean state."""
    if target.name == "sim":
        transport = ProcessTransport(target.sim_path, target.expected_temp)
        dev = DeviceClient(transport, target.timeout_scale, restart=transport.restart)
        boot = dev.wait_for_boot(5)
    else:
        url = target.port or (WOKWI_URL if target.name == "wokwi" else None)
        if url is None:
            pytest.exit("--target serial needs --port (e.g. COM3 or /dev/ttyUSB0)", returncode=4)
        transport = SerialTransport(url)
        dev = DeviceClient(transport, target.timeout_scale)
        try:
            boot = dev.wait_for_boot(2)      # we may have connected after the BOOT line
        except DeviceTimeout:
            boot = dev.restart()             # so ask for a fresh one
    pytestconfig.stash[RUN_INFO]["fw"] = boot.fw
    yield dev
    transport.close()


def reset_state(dev: DeviceClient) -> None:
    """Known starting point: no stream, no fault, default rate, empty buffer,
    sensor OK, no leftover input or events."""
    dev.drain(0.05)
    dev.expect_ok("STREAM OFF")
    if dev.has_hooks():
        dev.expect_ok("FAULT I2C OFF")
    dev.expect_ok("RATE 1000")
    dev.expect_ok("CLEAR")
    if dev.status()["sensor"] != "OK":
        dev.wait_for_event("SENSOR_OK", 7)   # a fault test left it LOST: wait for the retry
    dev.drain(0.05)
    dev.clear_events()


@pytest.fixture(autouse=True)
def clean_device(request: pytest.FixtureRequest, device: DeviceClient) -> DeviceClient:
    """Before every test: record where its log starts, skip hook tests on a
    production build, and put the device into the known state."""
    request.node.stash[DEVICE] = device
    request.node.stash[LOG_START] = device.log_mark()
    if request.node.get_closest_marker("hooks") and not device.has_hooks():
        pytest.skip("production build: FAULT commands are not available")
    reset_state(device)
    return device


@pytest.fixture
def fresh_device(device: DeviceClient) -> DeviceClient:
    """Restart first: a new simulator process, or RESET on Wokwi / a board."""
    device.restart()
    reset_state(device)
    return device


# ---- reporting ----------------------------------------------------------------------

@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item: pytest.Item, call: pytest.CallInfo):
    """Attach the test's full serial conversation to its report (terminal + HTML)."""
    outcome = yield
    report = outcome.get_result()
    dev = item.stash.get(DEVICE, None)
    if dev is None or report.when not in ("call", "setup"):
        return
    if report.when == "setup" and not report.failed:
        return
    text = dev.format_log(item.stash.get(LOG_START, 0))
    report.sections.append(("serial conversation", text or "(nothing sent or received)"))


def pytest_html_report_title(report) -> None:
    report.title = "Embedded HIL test report"


def pytest_html_results_summary(prefix, summary, postfix, session) -> None:
    info = session.config.stash[RUN_INFO]
    prefix.append(
        f"<p><b>Target:</b> {info['target']} &nbsp; <b>Firmware:</b> {info['fw']} "
        f"&nbsp; <b>Commit:</b> {info['commit']}</p>"
    )
