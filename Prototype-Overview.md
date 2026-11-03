# GeoNest Child Safety IoT

**Feature: Allottee & Device Location Dashboard, Safe Zone Management & Health Monitoring**

---

## 🎯 Purpose

This is a geofencing and health tracking dashboard for a college IoT project. The dashboard displays real-time location, environment, and health details of a child wearing a connected wristband. It integrates with a **Firebase Realtime Database** to stream data. Parents/guardians can manage multiple safe zones on a Google Map, monitor health vitals, trigger SOS alerts, and receive smart automated SMS during emergencies.

---

## 📊 Data Displayed

The dashboard fetches dynamic telemetry from the Firebase Realtime Database and manages safety zones from the web, while securely hardcoding static profile configuration on the client side:

- **Demo Profile** — Placeholder identity and contact fields
- **Device Info** — Unique ID and connection status (detects if offline for > 2 mins; data expires after 15 mins of staleness)
- **Live Location** — Real-time latitude and longitude streamed from the device (when outdoors)
- **Indoor Detection** — Displays the current room based on ESP-NOW RSSI tracking (mesh network) instead of GPS when indoors
- **Zone Status** — Whether the child is currently INSIDE or OUTSIDE the designated safe zones (zones sync with RTDB via website)
- **Health Vitals** — Heart Rate (bpm), O2 Saturation (SpO2 %), Body Temperature (°C), and Humidity
- **Posture & Activity** — Track current posture ("Standing", "Sitting", "Fallen!") and activity ("Standing", "Sitting", "Running", "Idle")
- **SOS Alert** — Physical SOS button on the wristband triggers immediate SMS with vitals and GPS to the parent

---

## 🗺️ Map Display & Safe Zones Logic

### Map View

- Renders a **Google Map** centered on the child's location or their safe zones
- Places a **live marker** showing the child's current coordinates
- Color-coded pins: **Green** (Inside), **Red** (Outside), **Yellow** (No Zone/Unknown)
- Draws distinct circles representing active safe boundaries
- Shows "Waiting for Device" placeholder when no location data is available

### ✅ Safe Zone Management (Up to 3 Zones)

#### Creating a Zone

1. Click any point on the Google Map — that point becomes the **center of the safe zone**
2. Specify the radius in the controls below the map (10m to 10,000m)
3. A preview circle is **drawn instantly** with dashed orange border
4. Click **"💾 Save New Zone"** to store it in the Firebase Realtime Database

#### Editing the Zone

1. Click the **"✏️ Edit"** button on an existing zone
2. The circle becomes **draggable** — drag the center to relocate
3. Drag the circle edges to resize the radius
4. Click **"✅ Ensure Changes"** to save updates to Firebase

#### Deleting the Zone

- Click the **"🗑️ Del"** button to remove a zone after confirmation
- Zone is immediately removed from Firebase and the map

---

## 🚨 Alert Behaviour & Smart Conditions

The system features a prioritized smart alerting banner for various situational and health-related emergencies:

| Priority | Alert Type | Trigger Condition | SMS Behavior |
|----------|------------|-------------------|--------------|
| 0 | `SOS` | SOS button pressed on wristband | 🆘 **Always sends** (bypass cooldown) |
| 1 | `HEALTH_EMERGENCY` | Posture = "Fallen!" **AND** SpO2 < 90% | 🏥 **Always sends** (bypass cooldown) |
| 2 | `FALL` | Posture = "Fallen!" **AND** SpO2 ≥ 90% | 🚑 **Always sends** (bypass cooldown) |
| 3 | `HEAT_ALERT` | Temperature > 38°C | 🔥 Subject to 30-min cooldown |
| 4 | `SPO2_LOW` | SpO2 < 90% (with no movement) | 🩸 Subject to 30-min cooldown |
| 5 | `HEALTH_DANGER` | Posture = "Standing" **AND** HR > 120 bpm | 💓 Dashboard only (no SMS) |
| 6 | `ZONE_BREACH` | Child exits ALL safe zones | ⚠️ Dashboard only (no SMS) |
| 7 | `ZONE_RETURN` | Child returns to safe zone | ✅ Dashboard only (no SMS) |
| — | `HOURLY_UPDATE` | Every 60 minutes (automatic) | 📱 **Always sends** (independent timer) |

> **Note:** When `HEAT_ALERT` or `SPO2_LOW` is suppressed due to the 30-minute cooldown, the alert is logged to Firebase (`suppressedAlerts` field) for dashboard visibility.

> **Important:** `FALL` and `HEALTH_EMERGENCY` are **mutually exclusive** — a fall with low SpO2 produces only `HEALTH_EMERGENCY`, never both simultaneously.

### Alert Priority System

- Only the **highest priority** alert is displayed in the main banner
- All active alerts are shown as chips below the banner for situational awareness
- Alert banner auto-dismisses when condition is resolved
- The **status indicator dot** pulses with a ping animation during `OUTSIDE`, `FALLEN`, and `HEALTH_EMERGENCY` states

### SOS Button Behaviour (Hardware)

When the child presses the SOS button on the wristband:
1. The wristband detects the button press on GPIO 35 (active LOW, debounced 2s)
2. Composes an SMS containing current vitals (Temp, Humidity, HR, SpO2) and a Google Maps link with GPS coordinates
3. Sends the SMS to the configured parent phone number via SIM800L
4. Uploads an emergency payload to Firebase with posture set to `"SOS TRIGGERED"`
5. Enters a 15-second cooldown before accepting another SOS press

### Hourly SMS Updates (Automated)

Every hour, the wristband automatically sends an SMS to the parent with:
- **Message prefix:** "Know your child doings!"
- Temperature, Heart Rate, SpO2, Posture, and Activity
- GPS coordinates with Google Maps link (if available)
- Indoor location note if GPS is not fixed

### SMS Rate Limiting (Anti-Spam Protection)

To prevent parents from receiving too many messages, the system uses a **30-minute global cooldown** for non-critical alerts:

| Priority | Alerts | Cooldown Rule |
|----------|--------|---------------|
| **CRITICAL** | SOS button, Fall detection | Always send immediately (bypass cooldown) |
| **NORMAL** | Temperature > 38°C, SpO2 < 90% | Subject to 30-min global cooldown |
| **SCHEDULED** | Hourly update (60 min) | Always send on schedule (independent timer) |

**Suppressed Alert Logging:** When a non-critical alert is suppressed due to the cooldown, it's logged to Firebase under `suppressedAlerts` so parents can see it on the dashboard.

### SpO2 Hysteresis (Debouncing)

To prevent rapid alert flickering when SpO2 fluctuates around the 90% threshold:

- `HEALTH_EMERGENCY` **triggers** when SpO2 drops **below 90%**
- `HEALTH_EMERGENCY` **clears** only when SpO2 rises **above 92%**
- Between 90–92%, the alert **stays in its current state** (2% buffer zone)

### Zone Tracking

- GPS zone checking runs **independently** of indoor detection — a child detected indoors via ESP-NOW beacons still has their GPS coordinates checked against configured safe zones
- On **first page load**, zone status is initialized silently without triggering false `ZONE_BREACH` or `ZONE_RETURN` alerts
- Zone transition alerts only fire on actual **state changes** (inside → outside or vice versa)

### Two-Way Communication (SMS Trigger)

When a critical emergency occurs (**Fall**, **SOS**, **Zone Breach**, or **Temperature > 38°C**), the wristband:
1. Detects the emergency condition
2. Composes the appropriate SMS message with context (vitals, location)
3. Sends SMS via SIM800L to the parent's phone number
4. Uploads emergency data to Firebase via WiFi
5. The dashboard displays "📡 COMMAND SENT TO HARDWARE: TRIGGER SMS" indicator for Fall and Zone Breach events

> **SMS Cooldown:** Temperature and SpO2 alerts have both a 5-minute per-alert cooldown AND a 30-minute global cooldown (any non-critical SMS resets the timer for all alert types). Fall and SOS alerts send immediately every time, bypassing all cooldowns.

---

## ⚙️ Firebase Integration

The project uses **Firebase Realtime Database** with `onValue` snapshot listeners for real-time data synchronization.

### Device Document Structure (`/demo-wristband`)

```json
{
  "latitude": 0,
  "longitude": 0,
  "lastUpdated": 1712567400000,
  "currentRoom": "Living Room",
  "roomRSSI": {
    "Living Room": -45,
    "Dining Room": -72,
    "Bed Room": -85,
    "Kitchen": -68
  },
  "lastRoomChange": 1712567400000,
  "heartRate": 85,
  "spo2": 98,
  "temperature": 36.5,
  "humidity": 45,
  "posture": "Standing",
  "activity": "Running",
  "zones": [
    {
      "center": { "lat": 0, "lng": 0 },
      "radius": 100
    }
  ]
}
```

> **Note:** The `lastUpdated` field is stored as a Unix timestamp (milliseconds) using Firebase's server timestamp `{".sv":"timestamp"}`. The dashboard converts this for display and data expiry calculations.

### Field Descriptions

| Field | Type | Source | Description |
|-------|------|--------|-------------|
| `latitude` | number | NEO-6M GPS | Outdoor location latitude |
| `longitude` | number | NEO-6M GPS | Outdoor location longitude |
| `lastUpdated` | number (Unix ms) | Firebase Server | Server-side timestamp of last data sync |
| `currentRoom` | string | Central Hub | Indoor room name from ESP-NOW mesh, or "Outdoors" |
| `roomRSSI` | object | Central Hub | Per-room signal strength (e.g., `{"Living Room": -45, "Dining Room": -72}`) |
| `lastRoomChange` | number (Unix ms) | Firebase Server | Timestamp of last room transition (only updated on room changes) |
| `heartRate` | number | MAX30102 | Heart rate in bpm |
| `spo2` | number | MAX30102 | Blood oxygen saturation percentage |
| `temperature` | number | DHT11 | Ambient temperature in °C |
| `humidity` | number | DHT11 | Ambient humidity percentage |
| `posture` | string | MPU-6050 | Body posture: "Standing", "Sitting", "Fallen!", or "SOS TRIGGERED" |
| `activity` | string | MPU-6050 | Current activity: "Standing", "Sitting", "Running", or "Idle" |
| `zones` | array | Web Dashboard | User-configured safe zones (max 3) |

> **Note:** Static parameters (`childName`, `childAge`, `parentContact`, `deviceId`) are hardcoded in the frontend (`Dashboard.jsx`) to simplify the payload from embedded hardware.

---

## 🖥️ Dashboard Layout

- **Top:** Smart priority Alert Banner (visible only during active alerts)
- **Left Panel (Info Panel - 380px):**
  - Child profile card with photo
  - Live status badge with color-coded indicator
  - Health vitals grid (HR, SpO2, Temp, Humidity)
  - Device ID and parent contact details
  - GPS coordinates OR indoor room display
  - List of configured Safe Zones
- **Right Panel (Map View - Flexible):**
  - Interactive Google Map with live marker
  - Geofence editing capabilities (click to create, drag to edit)
  - Zone controls overlay (save, edit, delete)

---

## 📡 Hardware Architecture

### System Overview

```
┌───────────────────────────────────────────────────────────────────────────┐
│                        CHILD WRISTBAND (ESP32)                            │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌──────────────┐   │
│  │ NEO-6M  │  │  DHT11  │  │ MPU-6050│  │MAX30102 │  │   SIM800L    │   │
│  │  GPS    │  │Temp/Hum │  │ Accel.  │  │ HR/SpO2 │  │   SMS Only   │   │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘  └──────┬───────┘   │
│       │           │           │           │              │             │
│       │         GPIO 15    I2C Bus 0    I2C Bus 1      UART           │
│       │           │       (GPIO 25/26) (GPIO 22/21)  (GPIO 18/19)     │
│       │           │           │           │              │             │
│       └───────────┴───────────┴───────────┴──────────────┘             │
│                              │            ┌───────────┐                │
│                         ESP32 ◄───────────┤ SOS Button│                │
│                    (Data Aggregator)       │ (GPIO 35) │                │
│                     │            │        └───────────┘                │
│              ESP-NOW Broadcast   WiFi → Firebase REST API              │
└──────────────────────┼───────────────────┼─────────────────────────────┘
                       │                   │
          ┌────────────┼───────────────────┘
          │            │
          │   ┌────────┼────────────────────┐
          │   │        │                    │
          ▼   ▼        ▼                    ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│  Living Room    │ │   Dining Room   │ │    Bed Room     │ │    Kitchen      │
│  ESP32-C3       │ │   ESP32-C3      │ │    ESP32-C3     │ │    ESP32-C3     │
│  (Room Scanner) │ │   (Room Scanner)│ │    (Room Scanner│ │    (Room Scanner│
└────────┬────────┘ └────────┬────────┘ └────────┬────────┘ └────────┬────────┘
         │                   │                   │                   │
         └───────────────────┼───────────────────┼───────────────────┘
                             │                   │
                             ▼                   │
                  ┌──────────────────┐           │
                  │  CENTRAL HUB     │           │
                  │  (ESP32 + WiFi)  │◄──────────┘
                  └────────┬─────────┘
                           │
                           │ Firebase RTDB PATCH
                           ▼
                  ┌──────────────────┐
                  │   Firebase       │
                  │   Realtime DB    │
                  └────────┬─────────┘
                           │
                           │ Real-time Sync
                           ▼
                  ┌──────────────────┐
                  │   Web Dashboard  │
                  │   (React App)    │
                  └──────────────────┘
```

### Communication Paths

| Path | Protocol | Purpose |
|------|----------|---------|
| Wristband → Room Scanners | ESP-NOW Broadcast (auto WiFi channel) | Identity ping every ~4s for RSSI measurement |
| Room Scanners → Central Hub | ESP-NOW Unicast (same WiFi channel as hub) | Forward RSSI + room name reports (x3 for reliability) |
| Central Hub → Firebase | WiFi → HTTPS PATCH | Update `currentRoom`, per-room RSSI, `lastUpdated`, `lastRoomChange` every 5s |
| Wristband → Firebase | WiFi → HTTPS PATCH | Upload sensor data (10s with GPS / 30s without GPS) + suppressed alert logs |
| Wristband → Parent Phone | SIM800L → GSM | **Critical alerts** (SOS, Fall) always send; **Non-critical** (Temp, SpO2) subject to 30-min global cooldown; **Hourly update** every 60 min |

### Hardware Components

#### 1. Child Wristband (ESP32)
| Sensor | Purpose | Interface | GPIO |
|--------|---------|-----------|------|
| NEO-6M | GPS location | UART (Serial1) | RX: GPIO 4, TX: GPIO 5 |
| DHT11 | Temp & Humidity | Single-wire | GPIO 15 |
| MPU-6050 | Posture/Activity | I2C Bus 0 | SDA: GPIO 25, SCL: GPIO 26 |
| MAX30102 | Heart Rate & SpO2 | I2C Bus 1 | SDA: GPIO 22, SCL: GPIO 21 |
| SIM800L | SMS alerts | UART (Serial2) | RX: GPIO 18, TX: GPIO 19 |
| SOS Button | Emergency panic | Digital Input | GPIO 35 (external 10K pull-up required) |

> **I2C Note:** MPU-6050 and MAX30102 are on **separate** I2C buses (`TwoWire(0)` and `TwoWire(1)`) to prevent signal interference.

> **Power Warning:** SIM800L requires 2A bursts — power directly from 3.7V LiPo battery, NOT from ESP32 pins.

#### 2. Room Scanners (4× ESP32-C3)
- Listen for wristband ESP-NOW broadcast pings
- Measure RSSI signal strength from the incoming packet metadata
- Forward RSSI + room name to Central Hub via ESP-NOW unicast (same WiFi channel, x3 for reliability)
- Each scanner has its own firmware file with a pre-configured `ROOM_NAME`

#### 3. Central Hub (ESP32)
- Connects to home WiFi
- Receives RSSI data from all room scanners via ESP-NOW
- Determines strongest room (highest RSSI) every 5 seconds
- Applies gradual RSSI decay (-3 per cycle) so stale rooms clear within ~60 seconds from typical signal strength
- Uses 2-cycle outdoors debounce — requires RSSI below -90 dBm for 2 consecutive evaluations before declaring "Outdoors" (prevents brief signal dips from false-triggering)
- Updates Firebase via HTTPS PATCH with `currentRoom`, per-room RSSI values, `lastUpdated` (server timestamp), and `lastRoomChange` (on room transitions only)
- If all RSSI values fall below -90 dBm for 2 consecutive cycles, assumes the child is "Outdoors"

### Indoor/Outdoor Operation Modes

| Mode | Detection | Data Upload Interval | Data Payload |
|------|-----------|---------------------|--------------|
| **Outdoor** | GPS valid (lat/lng ≠ 0) | Every 10 seconds | Full payload (GPS + vitals + posture + activity + `currentRoom` = "Outdoors") |
| **Indoor** | GPS invalid (no signal) | Every 30 seconds | Vitals-only payload (no GPS, `currentRoom` managed by Central Hub via ESP-NOW) |

> **Battery Optimization:** Indoor mode uploads 3× less frequently and omits GPS data, significantly extending wristband battery life.

> **Key Fix:** All devices auto-align to the same WiFi channel. The wristband uses `WIFI_AP_STA` mode with a soft AP on the same channel as the WiFi router, ensuring ESP-NOW broadcasts reach all room scanners. GPS coordinates are uploaded whenever valid—regardless of indoor/outdoor status. Room detection is handled entirely by the Central Hub via ESP-NOW RSSI triangulation.

---

