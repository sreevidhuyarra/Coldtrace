// Copy to secrets.h (git-ignored) and fill in.
// HUB_HOST must be the hub's DEVICE host name (az iot hub show --query properties.deviceHostName),
// "<hub>.device.azure-devices.net": newer hubs sign device SAS tokens for that name, and a token
// used against plain "<hub>.azure-devices.net" is refused with CONNACK 5 (not authorised).
#pragma once
#define HUB_HOST  "your-hub.device.azure-devices.net"
#define DEVICE_ID "coldtrace-01"
#define SAS_TOKEN "SharedAccessSignature sr=your-hub.device.azure-devices.net%2Fdevices%2Fcoldtrace-01&sig=...&se=..."
