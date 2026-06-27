# ESP32 Network Monitor

Ein vollständiges Netzwerk-Monitoring-System auf Basis des ESP32 WROOM-32.
Überwacht Hosts per Ping, scannt das lokale Netzwerk und zeigt alles in einem
modernen Web-Dashboard an – live über WebSocket.

---

## Features

- 📡 **Web-Dashboard** – serviert direkt vom ESP32, kein Cloud-Dienst nötig
- 🔁 **Live-Updates** – WebSocket-Verbindung für Echtzeit-Daten
- 📊 **Ping-Monitor** – überwacht mehrere Hosts gleichzeitig (Latenz, Status, Verlaufsgraph)
- 🔍 **LAN-Scanner** – findet aktive Geräte im Subnetz (/24)
- 💾 **Persistenz** – konfigurierte Hosts bleiben nach Neustart erhalten (NVS Flash)
- 🌐 **mDNS** – Dashboard erreichbar unter `http://esp32netmon.local`
- 🔌 **AP-Fallback** – startet automatisch einen Access Point falls WiFi fehlschlägt

---

## Voraussetzungen

- **Hardware**: ESP32 WROOM-32 (oder kompatibles Board)
- **Software**: VS Code + PlatformIO IDE Extension
- **Netzwerk**: 2.4 GHz WiFi

---

## Einrichtung (Schritt für Schritt)

### 1. WLAN-Daten eintragen

Öffne [`src/main.cpp`](src/main.cpp) und ändere die folgenden Zeilen:

```cpp
#define WIFI_SSID        "DEIN_WLAN_NAME"
#define WIFI_PASSWORD    "DEIN_WLAN_PASSWORT"
```

### 2. Firmware flashen

In VS Code PlatformIO (linke Seitenleiste):

```
Alien-Symbol → esp32dev → Upload
```

Oder in der PlatformIO Toolbar:
- **→ (Upload)** Button klicken

### 3. Dateisystem hochladen (Web-Dashboard)

⚠️ **Wichtig**: Dieser Schritt muss SEPARAT durchgeführt werden!

```
PlatformIO → esp32dev → Platform → Upload Filesystem Image
```

Oder via Terminal:
```bash
pio run --target uploadfs
```

### 4. Dashboard öffnen

1. Öffne den **Serial Monitor** (Stecker-Symbol in PlatformIO)
2. Warte auf die Ausgabe – du siehst die IP-Adresse:
   ```
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    Dashboard: http://192.168.x.x
    Oder:      http://esp32netmon.local
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   ```
3. Öffne die Adresse im Browser

---

## Dashboard-Bedienung

| Aktion | Beschreibung |
|--------|-------------|
| **+ Host hinzufügen** | Name + IP-Adresse eingeben, ESP32 pingt den Host alle 5s |
| **✕** (auf Host-Karte) | Host aus der Überwachung entfernen |
| **Scan starten** | Pingt alle 254 IPs im lokalen /24-Subnetz |
| **+ Add** (Scanner-Ergebnis) | Gefundenes Gerät direkt als überwachten Host hinzufügen |

---

## REST API (für externe Tools)

Der ESP32 stellt eine einfache JSON-API bereit:

| Methode | Endpoint | Beschreibung |
|---------|----------|-------------|
| `GET` | `/api/status` | Systemstatus (IP, RSSI, Uptime, Heap) |
| `GET` | `/api/hosts` | Liste aller überwachten Hosts |
| `POST` | `/api/hosts` | Host hinzufügen (`{"name":"Router","ip":"192.168.1.1"}`) |
| `DELETE` | `/api/hosts?id=XXXX` | Host entfernen |
| `POST` | `/api/scan/start` | LAN-Scan starten |
| `POST` | `/api/scan/stop` | LAN-Scan stoppen |

**WebSocket**: `ws://<ESP32-IP>/ws` – sendet `hosts`, `status` und `scan` Events

---

## Troubleshooting

**Build-Fehler mit ESPAsyncWebServer?**
Ersetze in `platformio.ini` die ersten zwei `lib_deps` Zeilen durch:
```ini
mathieucarbou/ESPAsyncWebServer @ ^3.3.0
```

**Dashboard lädt nicht?**
- Stelle sicher, dass du **Upload Filesystem Image** durchgeführt hast
- Prüfe im Serial Monitor ob LittleFS erfolgreich gemountet wurde

**WiFi verbindet nicht?**
- Der ESP32 startet automatisch einen AP (`ESP32-NetMon` / `netmon123`)
- Verbinde dich mit diesem AP und öffne `http://192.168.4.1`

**Ping funktioniert nicht?**
- Stelle sicher, dass der Zielhost ICMP nicht blockiert (Firewall!)
- Windows blockiert standardmäßig Pings – in der Windows-Firewall aktivieren

---

## Projektstruktur

```
ESP32-NetworkMonitor/
├── platformio.ini        ← PlatformIO Konfiguration
├── src/
│   └── main.cpp          ← ESP32 Firmware (Web-Server, Ping, Scanner)
├── data/
│   └── index.html        ← Web-Dashboard (wird auf LittleFS gespeichert)
└── README.md             ← Diese Datei
```
