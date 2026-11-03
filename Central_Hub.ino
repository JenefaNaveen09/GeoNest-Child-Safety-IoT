/*
 * GeoNest Band
 - CENTRAL HOME HUB (ESP32)
 * 
 * ROLE: Sits inside the house, connected to Home Wi-Fi. 
 * Listens via ESP-NOW for the 4 Room Scanners. Determines which room 
 * the child is in based on RSSI strength and updates Firebase instantly 
 * using high-speed home internet, bypassing the wristband's SIM800L.
 */

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#include <esp_now.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ==============================================================================
// CONFIGURATION
// ==============================================================================

// The structure that the Room Scanners will send to this Hub
typedef struct struct_message {
  char roomName[32];
  int rssi;
} struct_message;

struct_message incomingData;

// Memory to store the latest values from all 4 rooms
int livingRoomRSSI = -100;
int diningRoomRSSI = -100;
int bedRoomRSSI = -100;
int kitchenRSSI = -100;

unsigned long lastUploadTime = 0;

// --- STABILITY IMPROVEMENTS ---
#define EVAL_INTERVAL 5000       // Evaluate room every 5s (was 3s — too fast vs 4.2s ping rate)
#define RSSI_DECAY 3             // Decay by -3 per cycle (was -10 — too aggressive)
#define OUTDOORS_THRESHOLD -90   // RSSI below this = signal lost
#define OUTDOORS_DEBOUNCE 2      // Must be below threshold for N consecutive cycles before Outdoors

int outdoorsCount = 0;           // Debounce counter for Outdoors detection
String lastReportedRoom = "";    // Track room transitions for timestamp

// ==============================================================================
// ESP-NOW CALLBACK (When a Room Scanner sends data)
// ==============================================================================
void OnDataRecv(const esp_now_recv_info_t * info, const uint8_t *incomingDataPtr, int len) {
  // IMPORTANT: Only process packets that match our room report size
  // This filters out wristband broadcast pings (20 bytes) vs room reports (36 bytes)
  if (len != sizeof(incomingData)) {
    Serial.print("Ignoring non-room packet (size: ");
    Serial.print(len);
    Serial.println(")");
    return;
  }

  memcpy(&incomingData, incomingDataPtr, sizeof(incomingData));
  
  String room = String(incomingData.roomName);
  int signal = incomingData.rssi;
  
  Serial.print("Data incoming from -> ");
  Serial.print(room);
  Serial.print(" | RSSI: ");
  Serial.println(signal);

  // Update our internal memory
  if (room == "Living Room") livingRoomRSSI = signal;
  else if (room == "Dining Room") diningRoomRSSI = signal;
  else if (room == "Bed Room") bedRoomRSSI = signal;
  else if (room == "Kitchen") kitchenRSSI = signal;
}

// ==============================================================================
// CLOUD UPLOAD LOGIC
// ==============================================================================
void updateFirebase(String currentRoom) {
  if(WiFi.status() == WL_CONNECTED) {
    if (!String(FIREBASE_HOST).startsWith("https://") ||
        String(FIREBASE_AUTH).isEmpty() || String(FIREBASE_ROOT_CA).isEmpty() ||
        time(nullptr) < 1700000000) return;
    WiFiClientSecure *client = new WiFiClientSecure;
    if(client) {

      client->setCACert(FIREBASE_ROOT_CA);
      HTTPClient http;

      // The path is now part of the FIREBASE_HOST constant
      String fullUrl = String(FIREBASE_HOST) + "?auth=" + FIREBASE_AUTH;

      http.begin(*client, fullUrl);
      http.addHeader("Content-Type", "application/json");

      // Build payload with currentRoom, per-room RSSI, and lastUpdated
      // Only add lastRoomChange timestamp when the room actually changes
      String payload = "{\"currentRoom\":\"" + currentRoom + "\""
                       ",\"roomRSSI\":{\"Living Room\":" + String(livingRoomRSSI)
                       + ",\"Dining Room\":" + String(diningRoomRSSI)
                       + ",\"Bed Room\":" + String(bedRoomRSSI)
                       + ",\"Kitchen\":" + String(kitchenRSSI) + "}"
                       ",\"lastUpdated\":{\".sv\":\"timestamp\"}";

      // Add lastRoomChange when room transitions occur
      if (currentRoom != lastReportedRoom && currentRoom != "Outdoors") {
        payload += ",\"lastRoomChange\":{\".sv\":\"timestamp\"}";
      }

      payload += "}";

      int httpResponseCode = http.PATCH(payload);

      if (httpResponseCode >= 200 && httpResponseCode < 300) {
        Serial.print("Firebase Update OK (");
        Serial.print(httpResponseCode);
        Serial.println(")");
      } else {
        Serial.print("Firebase Update FAILED: ");
        Serial.println(httpResponseCode);
      }

      http.end();
      delete client;
    }
  }
}

// ==============================================================================
// SETUP AND LOOP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Connect to Wi-Fi to establish internet connection for Webhook uploads.
  // This forces the Hub radio to use the router's assigned Wi-Fi channel.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "time.google.com");
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi!");

  Serial.println("=============================");
  Serial.println("GuardBand Central Hub v2");
  Serial.print("Hub MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("WiFi Channel: ");
  Serial.println(WiFi.channel());
  Serial.println("=============================");

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  // Register Callback
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("Listening for Room Scanners...");
}

void loop() {
  // Evaluate room every 5 seconds (aligned with ~4.2s ping rate)
  if (millis() - lastUploadTime > EVAL_INTERVAL) {
    // Find strongest room
    String strongestRoom = "Living Room";
    int maxRSSI = livingRoomRSSI;

    if (diningRoomRSSI > maxRSSI) { maxRSSI = diningRoomRSSI; strongestRoom = "Dining Room"; }
    if (bedRoomRSSI > maxRSSI) { maxRSSI = bedRoomRSSI; strongestRoom = "Bed Room"; }
    if (kitchenRSSI > maxRSSI) { maxRSSI = kitchenRSSI; strongestRoom = "Kitchen"; }

    // Outdoors hysteresis: require N consecutive cycles below threshold
    if (maxRSSI < OUTDOORS_THRESHOLD) {
      outdoorsCount++;
      if (outdoorsCount >= OUTDOORS_DEBOUNCE) {
        strongestRoom = "Outdoors";
      } else {
        // Not yet confirmed — keep the last known room
        strongestRoom = lastReportedRoom.length() > 0 ? lastReportedRoom : "Outdoors";
        Serial.print("Outdoors debounce (");
        Serial.print(outdoorsCount);
        Serial.print("/");
        Serial.print(OUTDOORS_DEBOUNCE);
        Serial.println(")");
      }
    } else {
      // Signal is good — reset debounce counter
      outdoorsCount = 0;
    }

    Serial.print("Strongest: ");
    Serial.print(strongestRoom);
    Serial.print(" | RSSI: ");
    Serial.print(maxRSSI);
    Serial.print(" | Outdoors count: ");
    Serial.println(outdoorsCount);

    // Upload to Firebase (only after evaluation, not during)
    updateFirebase(strongestRoom);

    // Track room transitions
    lastReportedRoom = strongestRoom;

    // Gentle decay so stale rooms clear gradually (~30 seconds from -55 to below -90)
    livingRoomRSSI -= RSSI_DECAY;
    diningRoomRSSI -= RSSI_DECAY;
    bedRoomRSSI -= RSSI_DECAY;
    kitchenRSSI -= RSSI_DECAY;

    lastUploadTime = millis();
  }

  delay(100);
}
