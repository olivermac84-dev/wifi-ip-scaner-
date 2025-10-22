/*
  esp32_cctv_scanner.ino (text-output)
  ESP32-C3 Super Mini - LAN CCTV scanner with BLE control and persistent WiFi credentials.

  Changes for this version:
  - All BLE/terminal output is plain text (human readable), not JSON.
  - Single-notify sends to avoid chunking issues.
  - Commands remain the same: WIFI:SSID,PASS | GETWIFI | IPRANGE | SCAN | STOP | STATUS | HELP
  - Includes ip_port field equivalent in plain text device lines (e.g. "DEVICE: 192.168.1.12:80 ...")
*/

#include <WiFi.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#define DEFAULT_WIFI_SSID    ""      // leave blank to require BLE-set
#define DEFAULT_WIFI_PASS    ""

// NUS (Nordic UART Service) UUIDs
#define NUS_SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_CHAR_RX_UUID        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_CHAR_TX_UUID        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

NimBLECharacteristic* pTxCharacteristic = nullptr;
NimBLECharacteristic* pRxCharacteristic = nullptr;
bool bleClientConnected = false;

Preferences prefs;

// control
String wifiSSID = DEFAULT_WIFI_SSID;
String wifiPass = DEFAULT_WIFI_PASS;
volatile bool doScan = false;
volatile bool stopScan = false;

const int connectTimeoutMsDefault = 200; // default per-port connect timeout (ms)
int connectTimeoutMs = connectTimeoutMsDefault;
const uint16_t scanPorts[] = {80, 8080, 8000, 5000, 88, 554, 37777};
const size_t scanPortsCount = sizeof(scanPorts) / sizeof(scanPorts[0]);

// Helper: send text via BLE as a single notification (adds newline)
void bleSendLine(const String &s) {
  if (!pTxCharacteristic) return;
  String msg = s;
  // ensure newline at end for terminal readability
  if (!msg.endsWith("\n")) msg += "\n";
  pTxCharacteristic->setValue((uint8_t*)msg.c_str(), msg.length());
  pTxCharacteristic->notify();
  delay(8); // short pause to ensure notify goes out
}

// convenience: send a simple message
void sendMsg(const String &text) {
  bleSendLine(String("MSG: ") + text);
}

void sendStatusText() {
  String conn = (WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected";
  String ip = WiFi.localIP().toString();
  String scanning = doScan ? "true" : "false";
  String s = String("STATUS: wifi=") + conn + " ip=" + ip + " scanning=" + scanning;
  bleSendLine(s);
}

// Helpers to convert IP <-> uint32
uint32_t ipToU32(const IPAddress &ip) {
  uint32_t x = 0;
  for (int i = 0; i < 4; i++) {
    x = (x << 8) | ip[i];
  }
  return x;
}
IPAddress u32ToIP(uint32_t v) {
  uint8_t b[4];
  for (int i = 3; i >= 0; i--) {
    b[i] = v & 0xFF;
    v >>= 8;
  }
  return IPAddress(b[0], b[1], b[2], b[3]);
}

// Compute and send IP range info in plain text
void sendIpRangeInfoText() {
  if (WiFi.status() != WL_CONNECTED) {
    bleSendLine("IPRANGE: error: WiFi not connected");
    return;
  }

  IPAddress localIP = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  uint32_t ip32 = ipToU32(localIP);
  uint32_t mask32 = ipToU32(mask);
  uint32_t network = ip32 & mask32;
  uint32_t hostBitsMask = (~mask32);
  uint32_t broadcast = network | hostBitsMask;

  uint32_t firstHost = (hostBitsMask >= 2) ? (network + 1) : network;
  uint32_t lastHost  = (hostBitsMask >= 2) ? (broadcast - 1) : broadcast;
  uint32_t totalUsable = 0;
  if (broadcast > network) {
    if (broadcast - network >= 3) totalUsable = (broadcast - network - 1);
    else if (broadcast - network == 2) totalUsable = 1;
    else totalUsable = 0;
  }

  IPAddress netIP = u32ToIP(network);
  IPAddress bcastIP = u32ToIP(broadcast);
  IPAddress firstIP = u32ToIP(firstHost);
  IPAddress lastIP = u32ToIP(lastHost);
  String ipRange = String(firstIP.toString()) + "-" + lastIP.toString();

  String out = String("IPRANGE: network=") + netIP.toString()
             + " broadcast=" + bcastIP.toString()
             + " first=" + firstIP.toString()
             + " last=" + lastIP.toString()
             + " hosts=" + String(totalUsable)
             + " range=" + ipRange;
  bleSendLine(out);
}

// BLE callbacks
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    bleClientConnected = true;
    Serial.println("BLE: client connected");
    sendMsg("BLE client connected");
  }
  void onDisconnect(NimBLEServer* pServer) {
    bleClientConnected = false;
    Serial.println("BLE: client disconnected");
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    std::string rx = pChar->getValue();
    if (rx.length() == 0) return;
    String s = String(rx.c_str());
    s.trim();
    Serial.println("BLE RX: " + s);

    // HELP
    if (s == "HELP") {
      bleSendLine("HELP: WIFI:SSID,PASS | GETWIFI | IPRANGE | SCAN | STOP | STATUS | HELP");
      return;
    }

    // GETWIFI
    if (s == "GETWIFI") {
      String ss = prefs.getString("ssid", "");
      String connected = (WiFi.status() == WL_CONNECTED) ? "true" : "false";
      String out = String("GETWIFI: ssid=") + ss + " connected=" + connected;
      bleSendLine(out);
      return;
    }

    // STATUS
    if (s == "STATUS") {
      sendStatusText();
      return;
    }

    // IPRANGE - return available area / usable hosts
    if (s == "IPRANGE") {
      sendIpRangeInfoText();
      return;
    }

    // STOP scanning
    if (s == "STOP") {
      stopScan = true;
      doScan = false;
      bleSendLine("SCAN: stopped");
      return;
    }

    // SCAN command
    if (s == "SCAN") {
      if (WiFi.status() != WL_CONNECTED) {
        bleSendLine("SCAN: cannot start, WiFi not connected. Set WIFI credentials first.");
      } else {
        stopScan = false;
        doScan = true;
        bleSendLine("SCAN: scheduled");
      }
      return;
    }

    // WIFI:SSID,PASS  (PASS may contain commas; we split on first comma after prefix)
    if (s.startsWith("WIFI:")) {
      String payload = s.substring(5);
      // find first comma
      int c = payload.indexOf(',');
      String ss = "";
      String pw = "";
      if (c >= 0) {
        ss = payload.substring(0, c);
        pw = payload.substring(c + 1);
      } else {
        ss = payload;
        pw = "";
      }
      ss.trim();
      pw.trim();
      // save
      prefs.putString("ssid", ss);
      prefs.putString("pass", pw);
      wifiSSID = ss;
      wifiPass = pw;
      bleSendLine(String("WIFI: saved ssid=") + ss);

      // attempt connect
      if (wifiSSID.length() > 0) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        bleSendLine("WIFI: attempting connect...");
        WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 20) {
          delay(500);
          tries++;
        }
        if (WiFi.status() == WL_CONNECTED) {
          String ok = String("WIFI: connected, ip=") + WiFi.localIP().toString();
          bleSendLine(ok);
        } else {
          bleSendLine("WIFI: connect failed");
        }
      } else {
        bleSendLine("WIFI: empty SSID saved (no connect attempt)");
      }
      return;
    }

    // Unknown
    bleSendLine(String("MSG: unknown command: ") + s);
  }
};

void setupBLE() {
  NimBLEDevice::init("ESP32-C3-CamScanner");
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);

  pRxCharacteristic = pService->createCharacteristic(
    NUS_CHAR_RX_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pTxCharacteristic = pService->createCharacteristic(
    NUS_CHAR_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY
  );
  pTxCharacteristic->addDescriptor(new NimBLE2902());

  pService->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(NUS_SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->start();
  Serial.println("BLE advertising started");
}

// simple WiFi connect at startup if credentials exist in prefs
void connectWiFiStartup() {
  String ss = prefs.getString("ssid", "");
  String pw = prefs.getString("pass", "");
  if (ss.length() == 0) {
    Serial.println("No saved WiFi credentials.");
    return;
  }
  wifiSSID = ss;
  wifiPass = pw;
  Serial.printf("Connecting to saved WiFi SSID: %s\n", wifiSSID.c_str());
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed (waiting for BLE credentials)");
  }
}

// Probe an HTTP-like port: send GET / and read first response headers
String probeHTTP(const IPAddress &ip, uint16_t port) {
  WiFiClient client;
  client.setTimeout((connectTimeoutMs / 1000) + 1);
  if (!client.connect(ip, port, connectTimeoutMs)) return "";
  String req = "GET / HTTP/1.0\r\nHost: " + ip.toString() + "\r\nConnection: close\r\n\r\n";
  client.print(req);
  String response = "";
  unsigned long start = millis();
  while (millis() - start < connectTimeoutMs && client.available()) {
    String line = client.readStringUntil('\n');
    response += line;
    response += "\n";
    if (line.length() <= 1) break;
    if (response.length() > 512) break;
  }
  client.stop();
  return response;
}

// Probe RTSP port with OPTIONS
String probeRTSP(const IPAddress &ip, uint16_t port) {
  WiFiClient client;
  client.setTimeout((connectTimeoutMs / 1000) + 1);
  if (!client.connect(ip, port, connectTimeoutMs)) return "";
  String req = "OPTIONS rtsp://" + ip.toString() + "/ RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: ESP32-C3-Scanner\r\n\r\n";
  client.print(req);
  String response = "";
  unsigned long start = millis();
  while (millis() - start < connectTimeoutMs && client.available()) {
    String line = client.readStringUntil('\n');
    response += line;
    response += "\n";
    if (response.length() > 512) break;
  }
  client.stop();
  return response;
}

// Send device text result (single-line)
void sendDeviceText(const IPAddress &ip, uint16_t port, const String &service, const String &banner) {
  String b = banner;
  b.replace("\r", " ");
  b.replace("\n", " ");
  b.replace("\"", "'");
  String ipStr = ip.toString();
  String ipPort = ipStr + ":" + String(port);
  String line = String("DEVICE: ") + ipPort + " [" + service + "]";
  if (b.length() > 0) line += " " + b;
  bleSendLine(line);
}

// Main scan routine: uses subnet mask to compute range and reports range info
void scanNetworkOnce() {
  if (WiFi.status() != WL_CONNECTED) {
    bleSendLine("SCAN: WiFi not connected, cannot scan");
    return;
  }

  IPAddress localIP = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  uint32_t ip32 = ipToU32(localIP);
  uint32_t mask32 = ipToU32(mask);
  uint32_t network = ip32 & mask32;
  uint32_t hostBitsMask = (~mask32);
  uint32_t broadcast = network | hostBitsMask;

  uint32_t firstHost = (hostBitsMask >= 2) ? (network + 1) : network;
  uint32_t lastHost  = (hostBitsMask >= 2) ? (broadcast - 1) : broadcast;
  uint32_t totalUsable = 0;
  if (broadcast > network) {
    if (broadcast - network >= 3) totalUsable = (broadcast - network - 1);
    else if (broadcast - network == 2) totalUsable = 1;
    else totalUsable = 0;
  }

  IPAddress netIP = u32ToIP(network);
  IPAddress bcastIP = u32ToIP(broadcast);
  IPAddress firstIP = u32ToIP(firstHost);
  IPAddress lastIP = u32ToIP(lastHost);
  String ipRange = String(firstIP.toString()) + "-" + lastIP.toString();

  String startLine = String("SCAN START: ip=") + localIP.toString()
                     + " mask=" + mask.toString()
                     + " network=" + netIP.toString()
                     + " broadcast=" + bcastIP.toString()
                     + " first=" + firstIP.toString()
                     + " last=" + lastIP.toString()
                     + " hosts=" + String(totalUsable)
                     + " range=" + ipRange;
  bleSendLine(startLine);

  // determine scan bounds
  uint32_t startHost = firstHost;
  uint32_t endHost = lastHost;

  uint32_t hostsCount = (endHost >= startHost) ? (endHost - startHost + 1) : 0;
  if (hostsCount == 0) {
    bleSendLine("SCAN: No usable hosts in subnet");
    bleSendLine("SCAN DONE");
    return;
  }
  if (hostsCount > 4096) {
    uint32_t localNetwork24 = (ip32 & 0xFFFFFF00);
    startHost = localNetwork24 + 1;
    endHost = localNetwork24 + 254;
    hostsCount = 254;
    bleSendLine(String("SCAN: Large subnet detected, falling back to /24 scan: ") + u32ToIP(localNetwork24).toString() + "/24");
  }

  uint32_t scanned = 0;
  uint32_t total = hostsCount;

  for (uint32_t candidate = startHost; candidate <= endHost; candidate++) {
    if (stopScan) {
      bleSendLine("SCAN: stopped by user");
      stopScan = false;
      return;
    }

    if (candidate == ip32) {
      scanned++;
      if ((scanned % 10) == 0) {
        bleSendLine(String("PROGRESS: ") + String(scanned) + "/" + String(total));
      }
      continue; // skip own IP
    }

    IPAddress addr = u32ToIP(candidate);
    scanned++;

    if ((scanned % 10) == 0) {
      bleSendLine(String("PROGRESS: ") + String(scanned) + "/" + String(total));
    }

    // try ports
    for (size_t p = 0; p < scanPortsCount; p++) {
      uint16_t port = scanPorts[p];
      WiFiClient c;
      c.setTimeout((connectTimeoutMs / 1000) + 1);
      if (!c.connect(addr, port, connectTimeoutMs)) {
        continue;
      }
      c.stop(); // connected; perform a more specific probe to get banner
      String banner = "";
      String svc = "UNKNOWN";
      if (port == 554) {
        banner = probeRTSP(addr, port);
        svc = "RTSP";
      } else {
        banner = probeHTTP(addr, port);
        svc = "HTTP";
      }
      if (banner.length() == 0) {
        sendDeviceText(addr, port, svc, "");
      } else {
        String firstLine = banner.substring(0, banner.indexOf('\n'));
        firstLine.trim();
        String serverHeader = "";
        int idx = banner.indexOf("Server:");
        if (idx >= 0) {
          int eol = banner.indexOf('\n', idx);
          if (eol > idx) serverHeader = banner.substring(idx + 7, eol);
        }
        String chosen = (firstLine.length() > 0) ? firstLine : serverHeader;
        if (chosen.length() == 0) chosen = banner;
        sendDeviceText(addr, port, svc, chosen);
      }
      // small delay between probing ports
      delay(10);
    }
    // tiny pause
    delay(3);
  }

  bleSendLine("SCAN DONE");
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("ESP32-C3 CCTV Scanner (text-output) starting...");

  // init prefs
  prefs.begin("cctv", false);

  // BLE first (so you can connect and send creds)
  setupBLE();

  // try to connect using saved credentials
  connectWiFiStartup();
}

void loop() {
  // run scan if flagged and WiFi is connected
  if (doScan && WiFi.status() == WL_CONNECTED) {
    doScan = false;
    scanNetworkOnce();
  }
  // idle
  delay(200);
}
