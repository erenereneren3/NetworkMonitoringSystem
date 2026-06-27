#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ESP32Ping.h>

#define WIFI_SSID        "DEIN_WLAN_NAME"
#define WIFI_PASSWORD    "DEIN_WLAN_PASSWORT"
#define MDNS_HOSTNAME    "esp32netmon"
#define PING_INTERVAL_MS  5000
#define SCAN_DELAY_MS     25
#define HISTORY_SIZE      20
#define MAX_HOSTS         20

struct PingHistory {
    float   data[HISTORY_SIZE] = {};
    uint8_t head  = 0;
    uint8_t count = 0;

    void push(float v) {
        data[head] = v;
        head = (head + 1) % HISTORY_SIZE;
        if (count < HISTORY_SIZE) count++;
    }

    int toArray(float* out) const {
        int start = (count < HISTORY_SIZE) ? 0 : head;
        for (int i = 0; i < count; i++)
            out[i] = data[(start + i) % HISTORY_SIZE];
        return count;
    }
};

struct MonitoredHost {
    char        id[9]      = {};
    char        name[64]   = {};
    char        ip[40]     = {};
    bool        online     = false;
    float       latency    = 0.0f;
    int         failStreak = 0;
    unsigned long lastPing = 0;
    PingHistory history;
};

AsyncWebServer    server(80);
AsyncWebSocket    ws("/ws");
Preferences       prefs;
SemaphoreHandle_t hostsMux;
SemaphoreHandle_t scanMux;

MonitoredHost hosts[MAX_HOSTS];
int           hostCount = 0;

volatile bool scanning      = false;
volatile int  scanProgress  = 0;
int           scanTotal     = 254;
String        scanFound[256];
int           scanFoundCount = 0;

String genId() {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08X", (uint32_t)esp_random());
    return String(buf);
}

void saveHosts() {
    prefs.begin("nm", false);
    prefs.putInt("n", hostCount);
    for (int i = 0; i < hostCount; i++) {
        JsonDocument doc;
        doc["id"]   = hosts[i].id;
        doc["name"] = hosts[i].name;
        doc["ip"]   = hosts[i].ip;
        String s;
        serializeJson(doc, s);
        prefs.putString(("h" + String(i)).c_str(), s);
    }
    prefs.end();
}

void loadHosts() {
    prefs.begin("nm", true);
    int n = prefs.getInt("n", 0);
    hostCount = min(n, MAX_HOSTS);
    for (int i = 0; i < hostCount; i++) {
        String s = prefs.getString(("h" + String(i)).c_str(), "{}");
        JsonDocument doc;
        deserializeJson(doc, s);
        String id = doc["id"] | genId();
        strlcpy(hosts[i].id,   id.c_str(),            sizeof(hosts[i].id));
        strlcpy(hosts[i].name, doc["name"] | "Host",   sizeof(hosts[i].name));
        strlcpy(hosts[i].ip,   doc["ip"]  | "0.0.0.0", sizeof(hosts[i].ip));
    }
    prefs.end();
}

void wsBroadcast(const String& json) {
    ws.textAll(json);
}

void broadcastStatus() {
    JsonDocument doc;
    doc["t"]    = "status";
    doc["ip"]   = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["up"]   = millis() / 1000UL;
    doc["heap"] = ESP.getFreeHeap();
    doc["ssid"] = WiFi.SSID();
    doc["mac"]  = WiFi.macAddress();
    doc["hosts"]= hostCount;
    String s; serializeJson(doc, s);
    wsBroadcast(s);
}

void broadcastHosts() {
    if (!xSemaphoreTake(hostsMux, pdMS_TO_TICKS(500))) return;
    JsonDocument doc;
    doc["t"] = "hosts";
    JsonArray arr = doc["d"].to<JsonArray>();
    for (int i = 0; i < hostCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = hosts[i].id;
        o["n"]  = hosts[i].name;
        o["ip"] = hosts[i].ip;
        o["up"] = hosts[i].online;
        o["ms"] = (float)round(hosts[i].latency * 10.0f) / 10.0f;
        o["fs"] = hosts[i].failStreak;
        float hist[HISTORY_SIZE];
        int cnt = hosts[i].history.toArray(hist);
        JsonArray ha = o["h"].to<JsonArray>();
        for (int j = 0; j < cnt; j++)
            ha.add((float)round(hist[j] * 10.0f) / 10.0f);
    }
    xSemaphoreGive(hostsMux);
    String s; serializeJson(doc, s);
    wsBroadcast(s);
}

void broadcastScan() {
    JsonDocument doc;
    doc["t"]      = "scan";
    doc["active"] = (bool)scanning;
    doc["prog"]   = (int)scanProgress;
    doc["total"]  = scanTotal;
    if (xSemaphoreTake(scanMux, pdMS_TO_TICKS(200))) {
        JsonArray arr = doc["found"].to<JsonArray>();
        for (int i = 0; i < scanFoundCount; i++) arr.add(scanFound[i]);
        xSemaphoreGive(scanMux);
    }
    String s; serializeJson(doc, s);
    wsBroadcast(s);
}

void pingTask(void*) {
    struct Target { char ip[40]; char id[9]; };
    struct Result { char id[9]; bool ok; float ms; };

    while (true) {
        Target targets[MAX_HOSTS];
        int    tCount = 0;
        unsigned long now = millis();

        if (xSemaphoreTake(hostsMux, pdMS_TO_TICKS(1000))) {
            for (int i = 0; i < hostCount; i++) {
                if (now - hosts[i].lastPing >= PING_INTERVAL_MS) {
                    hosts[i].lastPing = now;
                    strlcpy(targets[tCount].ip, hosts[i].ip, 40);
                    strlcpy(targets[tCount].id, hosts[i].id, 9);
                    tCount++;
                }
            }
            xSemaphoreGive(hostsMux);
        }

        if (tCount > 0) {
            Result results[MAX_HOSTS];
            for (int i = 0; i < tCount; i++) {
                bool  ok = Ping.ping(targets[i].ip, 1);
                float ms = ok ? (float)Ping.averageTime() : -1.0f;
                strlcpy(results[i].id, targets[i].id, 9);
                results[i].ok = ok;
                results[i].ms = ms;
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            if (xSemaphoreTake(hostsMux, pdMS_TO_TICKS(1000))) {
                for (int r = 0; r < tCount; r++) {
                    for (int i = 0; i < hostCount; i++) {
                        if (strncmp(hosts[i].id, results[r].id, 8) == 0) {
                            hosts[i].online    = results[r].ok;
                            hosts[i].latency   = results[r].ms;
                            hosts[i].failStreak= results[r].ok ? 0 : hosts[i].failStreak + 1;
                            hosts[i].history.push(results[r].ms);
                            break;
                        }
                    }
                }
                xSemaphoreGive(hostsMux);
            }
            broadcastHosts();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void scanTask(void*) {
    if (xSemaphoreTake(scanMux, portMAX_DELAY)) {
        scanFoundCount = 0;
        xSemaphoreGive(scanMux);
    }

    IPAddress local = WiFi.localIP();
    scanTotal    = 254;
    scanProgress = 0;
    broadcastScan();

    for (int i = 1; i <= 254 && scanning; i++) {
        scanProgress = i;

        if ((uint8_t)i != local[3]) {
            IPAddress target(local[0], local[1], local[2], (uint8_t)i);
            if (Ping.ping(target, 1)) {
                if (xSemaphoreTake(scanMux, pdMS_TO_TICKS(500))) {
                    if (scanFoundCount < 256)
                        scanFound[scanFoundCount++] = target.toString();
                    xSemaphoreGive(scanMux);
                }
            }
        }

        if (i % 5 == 0) broadcastScan();
        vTaskDelay(pdMS_TO_TICKS(SCAN_DELAY_MS));
    }

    scanning = false;
    broadcastScan();
    Serial.printf("[Scan] Done. Found %d device(s)\n", scanFoundCount);
    vTaskDelete(NULL);
}

void statusTask(void*) {
    while (true) {
        broadcastStatus();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
               AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] Client #%u verbunden\n", client->id());
        broadcastStatus();
        broadcastHosts();
        broadcastScan();
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[WS] Client #%u getrennt\n", client->id());
    }
}

void setupAPI() {
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["ip"]     = WiFi.localIP().toString();
        doc["rssi"]   = WiFi.RSSI();
        doc["uptime"] = millis() / 1000UL;
        doc["heap"]   = ESP.getFreeHeap();
        doc["ssid"]   = WiFi.SSID();
        doc["mac"]    = WiFi.macAddress();
        doc["hosts"]  = hostCount;
        String s; serializeJson(doc, s);
        r->send(200, "application/json", s);
    });

    server.on("/api/hosts", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (!xSemaphoreTake(hostsMux, pdMS_TO_TICKS(1000))) { r->send(503); return; }
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < hostCount; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["id"]     = hosts[i].id;
            o["name"]   = hosts[i].name;
            o["ip"]     = hosts[i].ip;
            o["online"] = hosts[i].online;
            o["latency"]= hosts[i].latency;
        }
        xSemaphoreGive(hostsMux);
        String s; serializeJson(doc, s);
        r->send(200, "application/json", s);
    });

    auto* addHandler = new AsyncCallbackJsonWebHandler("/api/hosts",
        [](AsyncWebServerRequest* r, JsonVariant& body) {
            if (hostCount >= MAX_HOSTS) {
                r->send(400, "application/json", "{\"error\":\"Maximale Host-Anzahl erreicht\"}");
                return;
            }
            const char* name = body["name"] | "";
            const char* ip   = body["ip"]   | "";
            if (!name[0] || !ip[0]) {
                r->send(400, "application/json", "{\"error\":\"Name und IP erforderlich\"}");
                return;
            }
            if (xSemaphoreTake(hostsMux, pdMS_TO_TICKS(2000))) {
                int idx = hostCount++;
                String id = genId();
                strlcpy(hosts[idx].id,    id.c_str(), sizeof(hosts[idx].id));
                strlcpy(hosts[idx].name,  name,        sizeof(hosts[idx].name));
                strlcpy(hosts[idx].ip,    ip,          sizeof(hosts[idx].ip));
                hosts[idx].online     = false;
                hosts[idx].latency    = 0;
                hosts[idx].failStreak = 0;
                hosts[idx].lastPing   = 0;
                xSemaphoreGive(hostsMux);
            }
            saveHosts();
            r->send(200, "application/json", "{\"ok\":true}");
            broadcastHosts();
        });
    server.addHandler(addHandler);

    server.on("/api/hosts", HTTP_DELETE, [](AsyncWebServerRequest* r) {
        if (!r->hasParam("id")) {
            r->send(400, "application/json", "{\"error\":\"id Parameter fehlt\"}");
            return;
        }
        String id = r->getParam("id")->value();
        bool found = false;

        if (xSemaphoreTake(hostsMux, pdMS_TO_TICKS(2000))) {
            for (int i = 0; i < hostCount; i++) {
                if (String(hosts[i].id) == id) {
                    for (int j = i; j < hostCount - 1; j++) hosts[j] = hosts[j+1];
                    hostCount--;
                    found = true;
                    break;
                }
            }
            xSemaphoreGive(hostsMux);
        }

        if (found) {
            saveHosts();
            r->send(200, "application/json", "{\"ok\":true}");
            broadcastHosts();
        } else {
            r->send(404, "application/json", "{\"error\":\"Host nicht gefunden\"}");
        }
    });

    server.on("/api/scan/start", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (scanning) {
            r->send(409, "application/json", "{\"error\":\"Scan läuft bereits\"}");
            return;
        }
        scanning     = true;
        scanProgress = 0;
        xTaskCreatePinnedToCore(scanTask, "scan", 8192, NULL, 1, NULL, 0);
        r->send(200, "application/json", "{\"ok\":true}");
        Serial.println("[Scan] Gestartet");
    });

    server.on("/api/scan/stop", HTTP_POST, [](AsyncWebServerRequest* r) {
        scanning = false;
        r->send(200, "application/json", "{\"ok\":true}");
        Serial.println("[Scan] Gestoppt");
    });
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println(F("\n╔═══════════════════════════════════╗"));
    Serial.println(F("║   ESP32 Network Monitor  v1.0     ║"));
    Serial.println(F("╚═══════════════════════════════════╝\n"));

    if (!LittleFS.begin(true)) {
        Serial.println(F("[ERR] LittleFS konnte nicht gemountet werden!"));
    } else {
        Serial.println(F("[OK ] LittleFS gemountet"));
    }

    hostsMux = xSemaphoreCreateMutex();
    scanMux  = xSemaphoreCreateMutex();

    loadHosts();
    Serial.printf("[OK ] %d gespeicherte(r) Host(s) geladen\n", hostCount);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print(F("[...] WiFi verbindet"));
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[OK ] WiFi verbunden: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[OK ] Signal: %d dBm\n", WiFi.RSSI());

        if (MDNS.begin(MDNS_HOSTNAME)) {
            MDNS.addService("http", "tcp", 80);
            Serial.printf("[OK ] mDNS: http://%s.local\n", MDNS_HOSTNAME);
        }
    } else {
        Serial.println(F("[ERR] WiFi fehlgeschlagen – AP-Modus gestartet"));
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP32-NetMon", "netmon123");
        Serial.printf("[OK ] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
        Serial.println(F("[OK ] SSID: ESP32-NetMon  |  Passwort: netmon123"));
    }

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    setupAPI();

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest* r) {
        r->send(404, "text/plain", "404 Not Found");
    });

    server.begin();
    Serial.println(F("[OK ] HTTP-Server gestartet auf Port 80"));

    xTaskCreatePinnedToCore(pingTask,   "ping",   8192, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(statusTask, "status", 4096, NULL, 1, NULL, 1);

    Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
    Serial.print  (F(" Dashboard: http://"));
    Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf(" Oder:      http://%s.local\n", MDNS_HOSTNAME);
    Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"));
}

void loop() {
    ws.cleanupClients();
    delay(100);
}
