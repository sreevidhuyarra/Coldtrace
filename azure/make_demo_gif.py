"""Record docs/img/coldtrace-demo.gif: an excursion seen on the dashboard.

Drives the esp32_cloud firmware in Wokwi (rfc2217://localhost:4000) and the
device twin through `az`, screenshots the live dashboard with headless Edge at
each stage, and stitches the frames with Pillow. Run from the repo root:

    .venv/Scripts/python azure/make_demo_gif.py
"""
import json, subprocess, sys, tempfile, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "hil"))
from embedded_hil import DeviceClient, SerialTransport          # noqa: E402
from embedded_hil.cloud import CloudClient                       # noqa: E402
from PIL import Image                                            # noqa: E402

HUB, DEV, RG, APP = "coldtrace-hub-2db5", "coldtrace-01", "coldtrace-rg", "coldtrace-fn-2db5"
EDGE = next(p for p in (Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
                        Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe")) if p.exists())

cloud = CloudClient(HUB, DEV, RG, "coldtracest2db5")
key = cloud.az("functionapp", "keys", "list", "-n", APP, "-g", RG, "--query", "functionKeys.default", output="tsv")
url = f"https://{APP}.azurewebsites.net/api/dashboard?code={key}"
frames_dir = Path(tempfile.mkdtemp(prefix="coldtrace-gif-"))
profile = frames_dir / "edge-profile"


def shot(name: str) -> Path:
    out = frames_dir / f"{name}.png"
    subprocess.run([str(EDGE), "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--user-data-dir={profile}",
                    "--window-size=1100,720", "--virtual-time-budget=8000", f"--screenshot={out}", url],
                   check=True, capture_output=True)
    print("frame", name, flush=True)
    return out


dev = DeviceClient(SerialTransport("rfc2217://localhost:4000"), timeout_scale=4)
dev.send("")
for cmd in ("FAULT NET OFF", "FAULT NOACK OFF", "FAULT TEMP 4", "REPORT 2000"):
    dev.expect_ok(cmd, 5)
ver = cloud.twin_update({"limit_c": 8, "window_s": 20})
while int(dev.wait_for_event("TWIN", 30).args[0]) != ver:
    pass
since = __import__("datetime").datetime.now(__import__("datetime").timezone.utc)

frames = []
time.sleep(25)                                   # a run of cold readings
frames.append(shot("1-cold"))
dev.expect_ok("FAULT TEMP 12", 5)
time.sleep(12)
frames.append(shot("2-warming"))
cloud.wait_alert(lambda a: a["status"] == "OPEN", since, 180)
time.sleep(4)
frames.append(shot("3-alert"))
dev.expect_ok("FAULT TEMP 4", 5)
alert_id = cloud.alerts_since(since)[0]["RowKey"]
cloud.wait_alert(lambda a: a["RowKey"] == alert_id and a["status"] == "CLEARED", since, 120)
time.sleep(10)
frames.append(shot("4-cleared"))

# back to the cold-chain defaults
dev.expect_ok("REPORT 30000", 5)
cloud.twin_update({"report_ms": 30000, "limit_c": 8, "window_s": 300})

images = [Image.open(f).convert("P", palette=Image.ADAPTIVE, colors=128) for f in frames]
out = REPO / "docs" / "img" / "coldtrace-demo.gif"
images[0].save(out, save_all=True, append_images=images[1:], duration=[2200, 2200, 2800, 3000], loop=0,
               optimize=True)
print(f"wrote {out} ({out.stat().st_size // 1024} KB)")
