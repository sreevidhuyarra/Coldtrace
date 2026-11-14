"""embedded_hil - a small hardware-in-the-loop harness for the logger firmware.

Tests talk to a DeviceClient; the DeviceClient talks to a Transport. Swapping
the transport (host simulator process, Wokwi over RFC2217, or a real serial
port) is the only difference between targets.
"""
from .device import DeviceClient, DeviceError, DeviceTimeout
from .models import BootInfo, Event, Response, Sample
from .transport import ProcessTransport, SerialTransport, Transport

__all__ = [
    "BootInfo",
    "DeviceClient",
    "DeviceError",
    "DeviceTimeout",
    "Event",
    "ProcessTransport",
    "Response",
    "Sample",
    "SerialTransport",
    "Transport",
]
