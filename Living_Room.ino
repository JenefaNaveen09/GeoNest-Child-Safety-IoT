/*
 * GeoNest Band - ROOM SCANNER (ESP32-C3)
 * Room: Living Room
 * 
 * SIMPLIFIED APPROACH: No BLE needed!
 * The Wristband broadcasts ESP-NOW packets every 2 seconds.
 * This scanner LISTENS for those packets, reads the RSSI 
 * from the packet header, and forwards it to the Central Hub.
 * 
 * This avoids the BLE+WiFi crash on ESP32-C3.
 */

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ==============================================================================
// CONFIGURATION
// ==============================================================================
#define ROOM_NAME "Living Room"

// Your Central Hub MAC Address
uint8_t hubAddress[] = HUB_MAC;

// Packet the Wristband broadcasts
typedef struct wristband_ping {
  char deviceId[20];
} wristband_ping;

// Packet we send to the Hub
typedef struct room_report {
  char roomName[32];
  int rssi;
} room_report;

esp_now_peer_info_t hubPeer;

// ==============================================================================
// ESP-NOW RECEIVE CALLBACK (When the Wristband broadcasts a ping)
// ==============================================================================
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // Check if this is actually a wristband ping
  if (len == sizeof(wristband_ping)) {
    wristband_ping incoming;
    memcpy(&incoming, data, sizeof(incoming));
    
    // Verify it's our wristband
    if (String(incoming.deviceId) == "demo-wristband") {
      // Get RSSI from the packet metadata
      int rssi = info->rx_ctrl->rssi;
      
      Serial.print("Wristband detected! RSSI: ");
      Serial.println(rssi);

      // Forward to Hub — send 3 times with 50ms gaps to combat
      // Hub's ESP-NOW deaf periods during Firebase HTTP uploads
      room_report report;
      strcpy(report.roomName, ROOM_NAME);
      report.rssi = rssi;

      for (int i = 0; i < 3; i++) {
        esp_err_t result = esp_now_send(hubAddress, (uint8_t *)&report, sizeof(report));
        if (result == ESP_OK && i == 0) {
          Serial.println("Sent to Hub (x3)");
        }
        delay(50);  // Gap between repeats to let ESP-NOW stack transmit
      }
    }
  }
}

// ==============================================================================
// SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("=============================");
  Serial.println("GuardBand Room Scanner v2");
  Serial.print("Room: ");
  Serial.println(ROOM_NAME);
  Serial.println("=============================");

  // Connect to WiFi first — this sets the radio to the same channel as the Central Hub
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int wifiRetry = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetry < 30) {
    delay(500);
    Serial.print(".");
    wifiRetry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("WiFi Channel: ");
    Serial.println(WiFi.channel());
  } else {
    Serial.println("\nWiFi Failed — ESP-NOW will still work on current channel");
  }

  Serial.print("Scanner MAC: ");
  Serial.println(WiFi.macAddress());

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED!");
    return;
  }

  // Register Hub as peer
  memset(&hubPeer, 0, sizeof(hubPeer));
  memcpy(hubPeer.peer_addr, hubAddress, 6);
  hubPeer.channel = 0;
  hubPeer.encrypt = false;
  esp_now_add_peer(&hubPeer);

  // Listen for incoming broadcasts from the Wristband
  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("Ready! Listening for wristband pings...");
}

// ==============================================================================
// LOOP
// ==============================================================================
void loop() {
  // Reconnect WiFi if dropped — without WiFi, we're on the wrong channel
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost! Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
      delay(500);
      retry++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi reconnected! Channel: ");
      Serial.println(WiFi.channel());
    } else {
      Serial.println("WiFi reconnect failed");
    }
  }
  delay(5000);  // Check every 5 seconds
}
