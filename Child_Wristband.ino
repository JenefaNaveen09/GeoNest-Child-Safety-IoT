/*
 * GeoNest Band
 * SMART CHILD WRISTBAND (ESP32)
 *
 * CORE ARCHITECTURE:
 * - ESP-NOW Broadcaster: Broadcasts identity pings on channel 1 for Room Scanners.
 * - Central Hub: Manages indoor room detection via ESP-NOW RSSI triangulation.
 * - GPS + WiFi: Uploads coordinates to Firebase when GPS is valid (outdoors).
 * - FreeRTOS Dual Core: Core 1 = Sensors, Core 0 = Wireless Communication.
 * - SIM800L: SMS alerts for emergencies (Fall, SOS, Temperature, SpO2) + HOURLY updates.
 *
 * FEATURES:
 * - Activity Detection (Standing/Walking/Running) via accelerometer variance
 * - Temperature SMS alerts (>38°C)
 * - Smart health monitoring with SpO2 emergency detection
 * - Fall detection with immediate SMS + Firebase upload
 * - **HOURLY SMS UPDATE**: Every 60 minutes sends "Know your child doings!" with vitals + location
 *
 * INDOOR/OUTDOOR OPERATION:
 * - Outdoors: GPS valid → uploads every 10s with coordinates, currentRoom="Outdoors"
 * - Indoors: GPS invalid → uploads every 30s, Central Hub manages currentRoom via ESP-NOW
 */

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#include <Wire.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "DHT.h"

#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ==============================================================================
// CONFIGURATION & PINS
// ==============================================================================
#define DEVICE_ID "demo-wristband"

// Wi-Fi Credentials for outdoor connectivity (e.g. Mobile Hotspot)

// IMPORTANT: Paste the MAC address of your Central Home Hub here!
uint8_t hubAddress[] = HUB_MAC;

// Alert thresholds
#define TEMP_ALERT_THRESHOLD 38.0    // SMS alert if temp > 38°C
#define SPO2_EMERGENCY_THRESHOLD 90  // Emergency if SpO2 < 90%
#define HR_DANGER_THRESHOLD 120      // Danger if HR > 120 while standing

#define SIM800_RX 18
#define SIM800_TX 19
#define GPS_RX 4
#define GPS_TX 5
#define DHTPIN 15
#define DHTTYPE DHT11
#define SOS_PIN 35

// Objects
HardwareSerial SIM800(2);
HardwareSerial GPS_Serial(1);
TinyGPSPlus gps;
DHT dht(DHTPIN, DHTTYPE);
TwoWire I2COne = TwoWire(0);
TwoWire I2CTwo = TwoWire(1);
MAX30105 particleSensor;
Adafruit_MPU6050 mpu;

// FreeRTOS Cross-Core & State
volatile bool fallDetected = false;
volatile bool sosTriggered = false;
// NOTE: isIndoors removed - room detection now handled by Central Hub via currentRoom field
volatile int globalHeartRate = 0;
volatile int globalSpo2 = 0;
volatile float globalTemp = 0.0;
volatile float globalHumidity = 0.0;
volatile double globalLat = 0.0;
volatile double globalLng = 0.0;

// Activity Detection
String posture = "Standing";
String activity = "Standing";  // Standing, Walking, Running
volatile bool noMovement = false;

// Alert state tracking
volatile bool tempAlertSent = false;
volatile bool spo2AlertSent = false;
unsigned long lastTempAlertTime = 0;
unsigned long lastSpo2AlertTime = 0;
const unsigned long ALERT_COOLDOWN = 300000;  // 5 minutes between repeat alerts

// Hourly SMS update tracking
unsigned long lastHourlySMS = 0;
const unsigned long HOURLY_SMS_INTERVAL = 3600000;  // 1 hour = 60 * 60 * 1000 ms

// Global SMS Rate Limiter (prevent spam to parents)
unsigned long lastNonCriticalSMS = 0;
const unsigned long GLOBAL_SMS_COOLDOWN = 1800000;  // 30 minutes between non-critical SMS

// Suppressed alert tracking (logged to Firebase)
unsigned long lastSuppressedTempAlert = 0;
unsigned long lastSuppressedSpo2Alert = 0;
int suppressedAlertCount = 0;

// Heart rate beat detection (WORKING FORMAT)
long lastBeat = 0;
float beatsPerMinute;
int beatAvg;

#define RATE_SIZE 4
byte rates[RATE_SIZE];
byte rateSpot = 0;

unsigned long lastPrint = 0;

esp_now_peer_info_t peerInfo;

// Task Handles
TaskHandle_t SensorTask;
TaskHandle_t WirelessTask;

// ==============================================================================
// MOVEMENT TRACKING LOGIC (For SpO2 Emergency Checks)
// ==============================================================================
bool checkNoMovement(float currentAccel) {
  static float lastMagnitude = 0;
  static unsigned long lastMoveTime = 0;

  // Check if there's significant change in acceleration (>0.05g)
  if (abs(currentAccel - lastMagnitude) > 0.05) {
    lastMoveTime = millis();
  }
  lastMagnitude = currentAccel;

  // No movement for 30 seconds
  return (millis() - lastMoveTime) > 30000;
}

// ==============================================================================
// ESP-NOW PING CALLBACK (Deprecated - send success != reception)
// ==============================================================================
// NOTE: This callback only confirms transmission, NOT that a hub received it.
// We no longer use this for indoor/outdoor detection because it was unreliable.
// Indoor room detection is handled by the Central Hub via currentRoom field.
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // Deprecated - kept for compatibility but no longer sets isIndoors flag
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("✅ ESP-NOW Ping Sent (transmission confirmed)");
  } else {
    Serial.println("❌ ESP-NOW Ping Send Failed");
  }
}

// ==============================================================================
// CORE 1: REAL-TIME SENSORS (GPS, FALL DETECTION, VITALS, ACTIVITY)
// ==============================================================================
void TaskSensorRead(void *pvParameters) {
  for(;;) {
    // 1. Maintain GPS Buffer continually
    while (GPS_Serial.available() > 0) {
      if (gps.encode(GPS_Serial.read())) {
        if (gps.location.isValid()) {
          globalLat = gps.location.lat();
          globalLng = gps.location.lng();
        }
      }
    }

    // 1.5. Poll SOS Button (Active LOW with external pull-up)
    static unsigned long lastSosPress = 0;
    if (digitalRead(SOS_PIN) == LOW && (millis() - lastSosPress > 2000)) {
      sosTriggered = true;
      lastSosPress = millis();
    }

    // 2. Read MPU6050 for Fall Detection AND Activity Classification
    sensors_event_t a, g, tempEvent;
    mpu.getEvent(&a, &g, &tempEvent);

    // Convert internal m/s^2 to 'g' (Earth Gravity) to match your new logic
    float ax = a.acceleration.x / 9.81;
    float ay = a.acceleration.y / 9.81;
    float az = a.acceleration.z / 9.81;
    float totalAccel = sqrt(ax * ax + ay * ay + az * az);

    // 🚨 FALL & IMPACT DETECTION LOGIC
    if (totalAccel < 0.5) {
      posture = "Fallen!";     // Free Fall Detected ⚠️
      fallDetected = true;
    }
    else if (totalAccel > 2.5) {
      posture = "Fallen!";     // Impact Detected 💥 (Possible Fall)
      fallDetected = true;
    }
    // 🧍 POSTURE DETECTION
    else if (az > 0.8 && az < 1.2 && abs(ax) < 0.3 && abs(ay) < 0.3) {
      if (posture != "Fallen!") {
        posture = "Standing";
      }
      activity = "Standing";
    }
    else if (ax > 0.6 || ax < -0.6) {
      if (posture != "Fallen!") {
        posture = "Sitting";
      }
      activity = "Sitting";
    }
    // 🏃 ACTIVITY DETECTION
    else if (totalAccel > 1.3) {
      activity = "Running";
    }
    else {
      activity = "Idle";
    }

    noMovement = checkNoMovement(totalAccel);

    // 3. Read Vitals with WORKING FORMAT
    static unsigned long lastVitalRead = 0;

    // ===== DHT11 READ (Every 2 seconds) =====
    if(millis() - lastVitalRead > 2000) {
      float humidity = dht.readHumidity();
      float temperature = dht.readTemperature();

      // Check if reading failed
      if (isnan(humidity) || isnan(temperature)) {
        Serial.println("Failed to read from DHT sensor!");
      } else {
        globalTemp = temperature;
        globalHumidity = humidity;
      }

      lastVitalRead = millis();
    }

    // ===== MAX30102 READ (Continuous) =====
    long irValue = particleSensor.getIR();
    long redValue = particleSensor.getRed();

    // ✅ ONLY RESET ADDED (HR + SpO2)
    if (irValue < 10000) {
      beatAvg = 0;
      beatsPerMinute = 0;
      globalSpo2 = 0;
      globalHeartRate = 0;
    }

    // 🔴 FAST BEAT DETECTION (UNCHANGED)
    if (checkForBeat(irValue)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

      beatsPerMinute = 60 / (delta / 1000.0);

      if (beatsPerMinute > 30 && beatsPerMinute < 200) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;

        beatAvg = 0;
        for (byte i = 0; i < RATE_SIZE; i++)
          beatAvg += rates[i];

        beatAvg /= RATE_SIZE;
        globalHeartRate = beatAvg;
      }
    }

    // ===== SpO2 (UNCHANGED) =====
    float ratio = (float)redValue / (float)irValue;
    int estimatedSpO2 = 110 - (25 * ratio);
    if (estimatedSpO2 >= 70 && estimatedSpO2 <= 100) {
      globalSpo2 = estimatedSpO2;
    }

    // ===== PRINT EVERY 1 SECOND =====
    if (millis() - lastPrint > 1000) {
      lastPrint = millis();

      Serial.println("===== SENSOR DATA =====");
      Serial.print("Temperature: ");
      Serial.print(globalTemp);
      Serial.print(" °C  |  Humidity: ");
      Serial.print(globalHumidity);
      Serial.println(" %");
      Serial.println("===== MAX30102 =====");
      Serial.print("IR: "); Serial.println(irValue);
      Serial.print("Heart Rate: "); Serial.println(globalHeartRate);
      Serial.print("SpO2: "); Serial.println(globalSpo2);
      Serial.print("Lat: "); Serial.println(globalLat, 6);
      Serial.print("Lng: "); Serial.println(globalLng, 6);
      Serial.println("====================");
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ==============================================================================
// CORE 0: WIRELESS COMMUNICATION (BLE, ESP-NOW, SIM800L)
// ==============================================================================

void sendSMS(String text, int times = 2) {
  if (String(PARENT_PHONE_NUMBER).isEmpty()) return;
  for (int i = 0; i < times; i++) {
    Serial.print("📱 WAKING SIM800L FOR PHYSICAL SMS (Attempt ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.print(times);
    Serial.println(")");
    
    SIM800.println("AT+CMGF=1");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    SIM800.println("AT+CMGS=\"" + String(PARENT_PHONE_NUMBER) + "\"");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    SIM800.print(text);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    SIM800.write(26);
    vTaskDelay(6000 / portTICK_PERIOD_MS);
    Serial.println("✅ SMS Sent");

    if (i < times - 1) {
      vTaskDelay(2000 / portTICK_PERIOD_MS); // Brief pause before next SMS
    }
  }
}

void uploadToFirebase(String payload) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!String(FIREBASE_HOST).startsWith("https://") ||
        String(FIREBASE_AUTH).isEmpty() || String(FIREBASE_ROOT_CA).isEmpty() ||
        time(nullptr) < 1700000000) return;
    WiFiClientSecure *client = new WiFiClientSecure;
    if (client) {

      client->setCACert(FIREBASE_ROOT_CA);
      HTTPClient http;

      String fullUrl = String(FIREBASE_HOST) + "?auth=" + FIREBASE_AUTH;

      http.begin(*client, fullUrl);
      http.addHeader("Content-Type", "application/json");

      int httpResponseCode = http.PATCH(payload);
      if (httpResponseCode >= 200 && httpResponseCode < 300) {
        Serial.print("✅ Firebase Update Success, code: ");
        Serial.println(httpResponseCode);
      } else {
        Serial.print("❌ Firebase Update Failed, error: ");
        Serial.println(httpResponseCode);
      }
      http.end();
    }
    delete client;
  } else {
    Serial.println("❌ WiFi not connected. Re-attempting connection...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// ==============================================================================
// SMS RATE LIMITING HELPERS
// ==============================================================================

// Check if we can send a non-critical SMS (subject to 30-min cooldown)
bool canSendNonCriticalSMS() {
  return (millis() - lastNonCriticalSMS) > GLOBAL_SMS_COOLDOWN;
}

// Update the last non-critical SMS timestamp
void updateLastNonCriticalSMS() {
  lastNonCriticalSMS = millis();
}

// Log suppressed alert to Firebase for dashboard visibility
void logSuppressedAlertToFirebase(String alertType, int count) {
  if (WiFi.status() == WL_CONNECTED) {
    String payload = "{\"suppressedAlerts\":{\"type\":\"" + alertType +
                     "\",\"count\":" + String(count) +
                     ",\"lastSuppressed\":{\".sv\":\"timestamp\"}}}";
    uploadToFirebase(payload);
    Serial.println("📝 Suppressed alert logged to Firebase: " + alertType + " (count: " + String(count) + ")");
  }
}

void TaskWirelessComm(void *pvParameters) {
  for(;;) {
    // 0. SOS BUTTON PRIORITY OVERRIDE
    if (sosTriggered) {
      Serial.println("🚨 SOS TRIGGERED!");
      String smsText = "SOS ALERT!\nThe child pressed the SOS button!\n";
      smsText += "Your childs vitals:\n";
      smsText += "Temp: " + String(globalTemp) + " C\n";
      smsText += "Humidity: " + String(globalHumidity) + " %\n";
      smsText += "HR: " + String(globalHeartRate) + "\n";
      smsText += "SpO2: " + String(globalSpo2) + "\n";

      if (gps.location.isValid()) {
        smsText += "Location:\n";
        smsText += "https://maps.google.com/?q=";
        smsText += String(globalLat, 6) + "," + String(globalLng, 6);
      } else {
        smsText += "GPS not fixed";
      }

      sendSMS(smsText);

      // Force Upload via WiFi or Mobile Data
      String gpsFields = (globalLat != 0.0 && globalLng != 0.0) ?
                         (",\"latitude\":" + String(globalLat, 5) + ",\"longitude\":" + String(globalLng, 5)) : "";

      String sosPayload = "{\"posture\":\"SOS TRIGGERED\",\"activity\":\"" + activity + "\"" +
                         ",\"heartRate\":" + String(globalHeartRate) +
                         ",\"spo2\":" + String(globalSpo2) +
                         ",\"temperature\":" + String(globalTemp, 1) +
                         ",\"humidity\":" + String(globalHumidity, 1) +
                         gpsFields +
                         ",\"lastUpdated\":{\".sv\":\"timestamp\"}}";
      uploadToFirebase(sosPayload);

      sosTriggered = false;
      vTaskDelay(15000 / portTICK_PERIOD_MS);
      continue;
    }

    // 1. EMERGENCY PRIORITY OVERRIDE - Fall Detection
    if (fallDetected) {
      Serial.println("🚨 FALL DETECTED!");
      sendSMS("GuardBand ALERT: Severe Medical Fall Detected! Child needs immediate assistance.");

      // Force Upload via WiFi even if indoors using new JSON method
      String emergencyPayload = "{\"posture\":\"Fallen!\",\"activity\":\"" + activity + "\"" +
                               ",\"heartRate\":" + String(globalHeartRate) +
                               ",\"spo2\":" + String(globalSpo2) +
                               ",\"temperature\":" + String(globalTemp, 1) +
                               ",\"humidity\":" + String(globalHumidity, 1) +
                               ((globalLat != 0.0 && globalLng != 0.0) ? (",\"latitude\":" + String(globalLat, 5) + ",\"longitude\":" + String(globalLng, 5)) : "") +
                               ",\"lastUpdated\":{\".sv\":\"timestamp\"}}";
      uploadToFirebase(emergencyPayload);

      fallDetected = false;
      vTaskDelay(15000 / portTICK_PERIOD_MS);
      continue;
    }

    // 2. TEMPERATURE ALERT SMS (>38°C) - SUBJECT TO 30-MIN GLOBAL COOLDOWN
    if (globalTemp > TEMP_ALERT_THRESHOLD) {
      if (!tempAlertSent || (millis() - lastTempAlertTime) > ALERT_COOLDOWN) {
        if (canSendNonCriticalSMS()) {
          Serial.println("🔥 TEMPERATURE ALERT TRIGGERED!");
          updateLastNonCriticalSMS();
          sendSMS("GuardBand HEAT ALERT: Child's environment temperature is " + String(globalTemp, 1) + "C. Check on child immediately!");
          tempAlertSent = true;
          lastTempAlertTime = millis();
        } else {
          // Log suppressed alert to Firebase
          suppressedAlertCount++;
          lastSuppressedTempAlert = millis();
          logSuppressedAlertToFirebase("TEMPERATURE", suppressedAlertCount);
          Serial.println("⏭️ Temperature alert suppressed (within 30-min cooldown)");
        }
      }
    } else {
      tempAlertSent = false;
    }

    // 3. SPO2 EMERGENCY ALERT (<90% with no movement OR fallen posture) - SUBJECT TO 30-MIN GLOBAL COOLDOWN
    if (globalSpo2 > 0 && globalSpo2 < SPO2_EMERGENCY_THRESHOLD && (noMovement || posture == "Fallen!")) {
      if (!spo2AlertSent || (millis() - lastSpo2AlertTime) > ALERT_COOLDOWN) {
        if (canSendNonCriticalSMS()) {
          Serial.println("🏥 SPO2 EMERGENCY ALERT TRIGGERED!");
          updateLastNonCriticalSMS();
          sendSMS("GuardBand HEALTH EMERGENCY: SpO2 at " + String(globalSpo2) + "% with no movement. Seek medical help!");
          spo2AlertSent = true;
          lastSpo2AlertTime = millis();
        } else {
          // Log suppressed alert to Firebase
          suppressedAlertCount++;
          lastSuppressedSpo2Alert = millis();
          logSuppressedAlertToFirebase("SPO2", suppressedAlertCount);
          Serial.println("⏭️ SpO2 alert suppressed (within 30-min cooldown)");
        }
      }
    } else {
      spo2AlertSent = false;
    }

    // 4. HOURLY SMS UPDATE TO PARENTS (Every 1 hour)
    if (millis() - lastHourlySMS > HOURLY_SMS_INTERVAL) {
      lastHourlySMS = millis();
      Serial.println("🕐 SENDING HOURLY UPDATE TO PARENTS!");

      String hourlySms = "Know your child doings! | ";
      hourlySms += "Temp: " + String(globalTemp, 1) + "C | ";
      hourlySms += "HR: " + String(globalHeartRate) + "bpm | ";
      hourlySms += "SpO2: " + String(globalSpo2) + "% | ";
      hourlySms += "Posture: " + posture + " | ";
      hourlySms += "Activity: " + activity;

      if (gps.location.isValid()) {
        hourlySms += " | Location: ";
        hourlySms += String(globalLat, 6) + "," + String(globalLng, 6);
        hourlySms += " https://maps.google.com/?q=";
        hourlySms += String(globalLat, 6) + "," + String(globalLng, 6);
      } else {
        hourlySms += " | Location: GPS not fixed (Indoors)";
      }

      sendSMS(hourlySms);
      Serial.println("✅ Hourly SMS Sent");
    }

    // 5. BROADCAST IDENTITY PING TO ALL ROOM SCANNERS
    // All devices share the same WiFi channel now, so broadcast on current channel
    typedef struct wristband_ping {
      char deviceId[20];
    } wristband_ping;

    wristband_ping ping;
    strcpy(ping.deviceId, DEVICE_ID);

    uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_err_t broadcastResult = esp_now_send(broadcastAddr, (uint8_t *)&ping, sizeof(ping));
    if (broadcastResult == ESP_OK) {
      Serial.println("ESP-NOW Ping Sent OK");
    } else {
      Serial.print("ESP-NOW Ping FAILED: ");
      Serial.println(esp_err_to_name(broadcastResult));
    }

    // Wait briefly to let ESP-NOW packet transmit before any WiFi HTTP operations
    vTaskDelay(200 / portTICK_PERIOD_MS);

    static unsigned long lastGprsPoll = 0;

    // 5. GPRS DATA UPLOAD — Smart upload with GPS when valid
    // FIX: Always include GPS coordinates when they are valid, regardless of indoor/outdoor
    // The Central Hub manages currentRoom for indoor locations via ESP-NOW RSSI triangulation

    // Determine upload interval based on GPS validity (faster when GPS is active outdoors)
    bool hasValidGPS = (globalLat != 0.0 && globalLng != 0.0);
    unsigned long uploadInterval = hasValidGPS ? 10000 : 30000;  // 10s with GPS, 30s without

    if (millis() - lastGprsPoll > uploadInterval) {
      lastGprsPoll = millis();

      // Build GPS fields only if we have valid coordinates
      String gpsFields = "";
      String roomField = "";

      if (hasValidGPS) {
        Serial.println("📍 GPS valid: Uploading full payload with coordinates");
        gpsFields = ",\"latitude\":" + String(globalLat, 5) + ",\"longitude\":" + String(globalLng, 5);
        // Only set currentRoom to "Outdoors" if we have GPS and no indoor detection
        roomField = ",\"currentRoom\":\"Outdoors\"";
      } else {
        Serial.println("🏠 No GPS: Uploading vitals-only (Central Hub manages currentRoom)");
        // Don't include currentRoom - let Central Hub manage it via ESP-NOW
      }

      String payload = "{" + roomField +
                         "\"heartRate\":" + String(globalHeartRate) +
                         ",\"spo2\":" + String(globalSpo2) +
                         ",\"temperature\":" + String(globalTemp, 1) +
                         ",\"humidity\":" + String(globalHumidity, 1) +
                         ",\"posture\":\"" + posture + "\"" +
                         ",\"activity\":\"" + activity + "\"" +
                         gpsFields +
                         ",\"lastUpdated\":{\".sv\":\"timestamp\"}}";

      uploadToFirebase(payload);
    }

    vTaskDelay(4000 / portTICK_PERIOD_MS);
  }
}

// ==============================================================================
// SETUP (Bootloader)
// ==============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("GeoNest Band - Child Wristband Starting...");

  SIM800.begin(9600, SERIAL_8N1, SIM800_RX, SIM800_TX);
  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("✅ GPS Serial Initialized");

  // Initialize WiFi for Firebase communication (must set mode BEFORE begin)
  // Use WIFI_AP_STA so ESP-NOW broadcast works reliably (broadcast fails in pure STA mode)
  WiFi.mode(WIFI_AP_STA);
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "time.google.com");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("WiFi Channel: ");
    Serial.println(WiFi.channel());
    // Start soft AP on the same channel as STA — enables ESP-NOW broadcast
    // No device needs to connect to this AP; it just activates the broadcast path
    WiFi.softAP("GeoNest_Wristband", NULL, WiFi.channel());
    Serial.println("Soft AP started (enables ESP-NOW broadcast)");
  } else {
    Serial.println("\nWiFi Failed (Will retry in tasks)");
    // Start AP on default channel even if STA fails, so ESP-NOW broadcast still works
    WiFi.softAP("GeoNest_Wristband", NULL, 1);
  }

  // Initialize SIM800L for SMS ONLY
  Serial.println("Initializing SIM800L...");
  SIM800.println("AT");
  delay(100);
  SIM800.println("AT+CMGF=1"); // SMS text mode
  delay(100);
  Serial.println("✅ SIM800L Initialized");

  // Init Hardware Components
  // IMPORTANT: GPIO 35 is input-only with NO internal pull-ups!
  // External 10K pull-up resistor between GPIO 35 and 3V3 is REQUIRED.
  pinMode(SOS_PIN, INPUT);
  I2COne.begin(25, 26);
  I2CTwo.begin(22, 21);

  // Initialize DHT11
  Serial.println("DHT11 Test Starting...");
  dht.begin();
  Serial.println("✅ DHT11 Initialized");

  // Initialize MPU6050
  if (mpu.begin(0x68, &I2COne)) {
    Serial.println("✅ MPU6050 Initialized");
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  } else {
    Serial.println("❌ MPU6050 not found!");
  }

  // Initialize MAX30102 with working setup
  if (particleSensor.begin(I2CTwo, I2C_SPEED_FAST)) {
    Serial.println("✅ MAX30102 Sensor OK");
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x1F);
    particleSensor.setPulseAmplitudeIR(0x1F);
  } else {
    Serial.println("❌ MAX30102 Sensor not found!");
  }

  // Initialize Wireless Network
  Serial.println("Initializing ESP-NOW...");
  // WiFi.mode(WIFI_STA) already called before WiFi.begin() — do not call again here
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error initializing ESP-NOW");
    return;
  }
  Serial.println("✅ ESP-NOW Initialized");
  esp_now_register_send_cb(OnDataSent);

  // Register Central Hub as peer
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, hubAddress, 6);
  peerInfo.channel = 0;  // Use current WiFi channel
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    Serial.println("✅ Central Hub Peer Registered");
  }

  // Register Broadcast peer (for room scanner pings)
  esp_now_peer_info_t broadcastPeer;
  memset(&broadcastPeer, 0, sizeof(broadcastPeer));
  memset(broadcastPeer.peer_addr, 0xFF, 6);
  broadcastPeer.channel = 0;
  broadcastPeer.encrypt = false;
  if (esp_now_add_peer(&broadcastPeer) == ESP_OK) {
    Serial.println("✅ Broadcast Peer Registered");
  }

  delay(3000);

  // --- FREE-RTOS START ---
  Serial.println("Starting FreeRTOS Tasks...");
  xTaskCreatePinnedToCore(TaskSensorRead, "SensorCore", 10000, NULL, 1, &SensorTask, 1);
  xTaskCreatePinnedToCore(TaskWirelessComm, "WirelessCore", 10000, NULL, 1, &WirelessTask, 0);
  Serial.println("✅ FreeRTOS Tasks Started");
  Serial.println("====================");
}

void loop() { vTaskDelay(1000 / portTICK_PERIOD_MS); }