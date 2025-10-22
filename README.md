```markdown
# ESP32-C3 CCTV LAN Scanner (Text BLE Terminal)

Overview
---------
This project runs on an ESP32-C3 Super Mini and discovers likely CCTV/IP camera devices on your local network. The device is controlled and reports results over Bluetooth Low Energy (Nordic UART Service) using human-readable plain text lines (no JSON). Wi‑Fi credentials are stored persistently so the device can reconnect after power cycles.

This README explains what the device can do, how to use it from a BLE serial/terminal app, example terminal output, tuning options, troubleshooting, and useful next steps.

Key features
-------------
- Persistent Wi‑Fi credentials (Preferences) — set once via BLE and retained across reboots.
- BLE control + text output using Nordic UART Service (NUS) — works with nRF Connect, Serial BLE Terminal, or custom app.
- Commands to set/get Wi‑Fi, get IP range, start/stop scans, check status and get help.
- Computes subnet range (network, broadcast, first/last usable hosts) and reports the usable IP range.
- Scans the usable host range (with a safe fallback to /24 for extremely large subnets).
- Probes common camera ports and lightweight protocols:
  - HTTP-like: 80, 8080, 8000, 5000, 88
  - RTSP: 554
  - Other camera port: 37777 (Dahua)
- Probes are non-intrusive: HTTP GET / and RTSP OPTIONS (no logging in or brute-force).
- Plain text terminal output (one human-readable line per notification) to avoid parsing/concatenation problems.

Requirements
-------------
- ESP32-C3 Super Mini (or compatible ESP32-C3 board)
- Arduino IDE or PlatformIO with ESP32 Arduino core
- NimBLE-Arduino library (BLE)
- Preferences library (bundled with ESP32 core)

BLE (NUS) details
------------------
- Device name advertised: ESP32-C3-CamScanner
- Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
- RX (write) UUID: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E — write commands here
- TX (notify) UUID: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E — subscribe to receive text notifications

Recommended BLE apps: nRF Connect (Android/iOS), Serial Bluetooth Terminal apps that support BLE notifications.

BLE commands (send these exact ASCII strings to RX)
---------------------------------------------------
- WIFI:SSID,PASSWORD
  - Save Wi‑Fi credentials and attempt to connect.
  - Example: WIFI:HomeNet,mysecret
  - Terminal responses: "WIFI: saved ssid=HomeNet", "WIFI: attempting connect...", then "WIFI: connected, ip=192.168.1.25" or "WIFI: connect failed"

- GETWIFI
  - Returns saved SSID and whether Wi‑Fi is connected:
  - Example: "GETWIFI: ssid=HomeNet connected=true"

- IPRANGE
  - Returns computed subnet info and usable host range:
  - Example: "IPRANGE: network=192.168.1.0 broadcast=192.168.1.255 first=192.168.1.1 last=192.168.1.254 hosts=254 range=192.168.1.1-192.168.1.254"

- SCAN
  - Start scanning the network. The first response lists scan start and range details. During the scan you receive PROGRESS and DEVICE lines. When finished you receive "SCAN DONE".

- STOP
  - Stop a running scan. Response: "SCAN: stopped" or "SCAN: stopped by user"

- STATUS
  - Returns a short status line: "STATUS: wifi=connected ip=192.168.1.25 scanning=false"

- HELP
  - Returns a single help line with the supported commands.

Plain text responses — exact line prefixes
-------------------------------------------
The device sends plain text lines with clear prefixes to make parsing or reading easy in a terminal app:

- MSG: informational messages from the device
- WIFI: Wi‑Fi related messages
- IPRANGE: subnet / usable host range info
- SCAN START: scan initialization and range summary
- PROGRESS: periodic scan progress reports (e.g., "PROGRESS: 10/254")
- DEVICE: discovered device line (ip:port, service, banner)
  - Example: "DEVICE: 192.168.1.12:80 [HTTP] Server: GoAhead/3.5"
- SCAN DONE: scan completed
- SCAN: scan status messages (e.g., stopped, scheduled)
- STATUS: quick status overview
- HELP: list of commands

Example BLE terminal session (what you send → what you see)
-------------------------------------------------------------
1) Set Wi‑Fi:
You send:
WIFI:HomeNet,mysecret

You see:
WIFI: saved ssid=HomeNet
WIFI: attempting connect...
WIFI: connected, ip=192.168.1.25

2) Get IP range:
You send:
IPRANGE

You see:
IPRANGE: network=192.168.1.0 broadcast=192.168.1.255 first=192.168.1.1 last=192.168.1.254 hosts=254 range=192.168.1.1-192.168.1.254

3) Start a scan:
You send:
SCAN

You see:
SCAN START: ip=192.168.1.25 mask=255.255.255.0 network=192.168.1.0 broadcast=192.168.1.255 first=192.168.1.1 last=192.168.1.254 hosts=254 range=192.168.1.1-192.168.1.254
PROGRESS: 10/254
DEVICE: 192.168.1.12:80 [HTTP] Server: GoAhead/3.5
DEVICE: 192.168.1.15:554 [RTSP] RTSP/1.0 200 OK
PROGRESS: 20/254
...
SCAN DONE

4) Stop an ongoing scan:
You send:
STOP

You see:
SCAN: stopped

Why plain text (not JSON)?
---------------------------
- Plain text lines are immediately human-readable in BLE terminals and avoid issues where a BLE client concatenates or splits notification chunks incorrectly.
- The firmware still sends each line as a single BLE notification to reduce the chance of split fragments, and includes a combined ip:port in each DEVICE line for clarity.

Tuning parameters (edit in sketch)
----------------------------------
- Ports scanned: modify the scanPorts[] array to add or remove TCP ports to probe.
- connectTimeoutMs: increase for higher reliability on slow networks, or decrease for faster scans (at the cost of missed hosts).
- Large subnets: the sketch will fall back to scanning the device's /24 if the usable host count exceeds 4096 to prevent extremely long operations.

Limitations and cautions
-------------------------
- The tool performs discovery only — it uses light probes (HTTP GET /, RTSP OPTIONS). It does not authenticate, modify devices, or attempt brute-force operations.
- Accuracy depends on host responsiveness and the configured timeouts.
- ESP32-C3 supports 2.4 GHz Wi‑Fi only — ensure the router broadcasts a 2.4 GHz SSID.
- Only scan networks you own or have permission to scan. Unauthorized scanning may violate laws or policies.

Troubleshooting tips
---------------------
- BLE device not visible: ensure the board is powered and advertising; on Android give the BLE app location permissions if required.
- No notifications: confirm you subscribed to TX notifications in your BLE app.
- Wi‑Fi connection failed: verify SSID/password and 2.4 GHz availability; check signal strength.
- Scan seems slow: reduce connectTimeoutMs in the sketch or restrict the IP range to scan fewer addresses.
- Malformed output: the firmware sends whole lines as single notifications — if you still see broken lines, test with nRF Connect to confirm raw notifications.

Useful next steps and extensions
--------------------------------
If you want more capabilities, these are practical extensions I can implement:
- Toggleable output modes (TEXT or JSON) with a MODE command.
- ONVIF WS‑Discovery (UDP 3702 multicast) for canonical ONVIF device discovery.
- Concurrent scanning (multiple tasks and connection pooling) to speed up scans.
- A small mobile app (Flutter or native) to connect to the device, send commands, parse text lines, display a sortable device list and export CSV.
- Save last-scan results to SPIFFS/LittleFS and serve via HTTP for browser access.
- OTA firmware update support.

License
--------
MIT — include your preferred license in the repository.

---

This README describes what the device can do, how to use it from a BLE serial terminal, and how to tune and extend it. Flash the provided sketch to your ESP32-C3 Super Mini, connect with a BLE terminal app, and follow the "Example BLE terminal session" steps to try it out.
```
