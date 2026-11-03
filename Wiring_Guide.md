# GeoNest Band

This guide maps out EXACTLY how to connect all **6 devices** in the GeoNest Band mesh network.

---

## 🔋 Crucial Power Warning
> **IMPORTANT:** The SIM800L module consumes massive power (up to 2A bursts) when connecting to cell towers. **You MUST power the SIM800L directly from an external 3.7V LiPo Battery.** If you power it from the ESP32's 3V3/5V pins, the ESP32 will crash and reboot. All other sensors can safely run from the ESP32's 3V3 pin.

---

## Device 1: Child Wristband (ESP32)

This is the main tracker worn by the child. It connects to 5 peripherals and an SOS button.

### SIM800L (GSM Module — SMS Alerts Only)
| SIM800L Pin | Connects To |
|---|---|
| VCC | 3.7V LiPo Battery POSITIVE |
| GND | LiPo Battery NEGATIVE **AND** ESP32 GND (shared ground!) |
| TXD | ESP32 **GPIO 18** (RX — ESP32 receives from SIM800L) |
| RXD | ESP32 **GPIO 19** (TX — ESP32 transmits to SIM800L) |

> **Note:** The SIM800L is used **only for SMS alerts** (Fall, SOS, Temperature, SpO2 emergencies). All sensor data uploads to Firebase go through WiFi, not GPRS.

### NEO-6M (GPS Module)
| NEO-6M Pin | Connects To |
|---|---|
| VCC | ESP32 3V3 |
| GND | ESP32 GND |
| TX  | ESP32 **GPIO 4** (RX — ESP32 receives from NEO-6M) |
| RX  | ESP32 **GPIO 5** (TX — ESP32 transmits to NEO-6M) |

### DHT11 (Temperature & Humidity)
| DHT11 Pin | Connects To |
|---|---|
| VCC  | ESP32 3V3 |
| GND  | ESP32 GND |
| DATA | ESP32 **GPIO 15** (add 10K pull-up resistor between VCC and DATA) |

### MPU-6050 (Accelerometer / Fall Detection) — I2C Bus 0
| MPU-6050 Pin | Connects To |
|---|---|
| VCC | ESP32 3V3 |
| GND | ESP32 GND |
| SDA | ESP32 **GPIO 25** (I2C Bus 0 — Data) |
| SCL | ESP32 **GPIO 26** (I2C Bus 0 — Clock) |

### MAX30102 (Heart Rate & SpO2) — I2C Bus 1
| MAX30102 Pin | Connects To |
|---|---|
| VCC | ESP32 3V3 |
| GND | ESP32 GND |
| SDA | ESP32 **GPIO 22** (I2C Bus 1 — Data) |
| SCL | ESP32 **GPIO 21** (I2C Bus 1 — Clock) |

> **Note:** The MPU-6050 (Bus 0: GPIO 25/26) and MAX30102 (Bus 1: GPIO 22/21) are on **separate I2C buses** to prevent signal interference. The ESP32's `TwoWire(0)` and `TwoWire(1)` handle them independently.

### SOS Button
| Button Pin | Connects To |
|---|---|
| Pin 1 | ESP32 **GPIO 35** |
| Pin 2 | ESP32 GND |

> **IMPORTANT RESISTOR REQUIREMENT:** GPIO 35 is an input-only pin without internal pull-ups! You **MUST** connect a 10K ohm external pull-up resistor between the GPIO 35 pin and the 3V3 power rail, so the pin rests at 3.3V (HIGH) when unpressed, and drops to GND (LOW) when pressed.

---

## Device 2: Central Home Hub (ESP32)

This ESP32 sits plugged into a wall outlet near your home Wi-Fi router. It has **NO external sensors** — it only uses its built-in Wi-Fi and ESP-NOW radio.

| What | Details |
|---|---|
| Power | USB cable plugged into a wall charger or power bank |
| Wi-Fi | Connects to your home router (set SSID and password in the local secrets.h) |
| ESP-NOW | Built-in, no wiring needed |
| Antenna | Use the PCB antenna on the ESP32 board itself |

### Setup Steps:
1. Flash `Central_Hub/Central_Hub.ino` to this ESP32.
2. Open Arduino IDE **Serial Monitor** (115200 baud).
3. It will print: `Hub MAC: XX:XX:XX:XX:XX:XX`
4. **Copy this MAC address** and paste it into `HUB_MAC` in each sketch’s local `secrets.h` in:
   - Each Room Beacon firmware file (e.g., `Living_Room/Living_Room.ino` line 25)
   - `Child_Wristband/Child_Wristband.ino` (line 56)

Example: If the Serial Monitor prints `70:4B:CA:8E:DA:C4`, you write:
```cpp
#define HUB_MAC {0, 0, 0, 0, 0, 0}
```

> **Important:** The Central Hub uses **gradual RSSI decay (-3 per 5-second cycle)** and **2-cycle outdoors debounce** to prevent false "Outdoors" detection. This means it requires 2 consecutive evaluations with all RSSI values below -90 dBm before declaring "Outdoors". Stale room signals decay from typical strength (-55 dBm) to below -90 dBm in about 12 cycles (~60 seconds).

---

## Devices 3–6: Room Scanners (4× ESP32-C3)

Each ESP32-C3 sits in one room. They have **NO external sensors** — they only listen for ESP-NOW broadcast pings from the wristband and forward RSSI data to the Central Hub.

| What | Details |
|---|---|
| Power | USB cable plugged into a wall charger in each room |
| ESP-NOW | Built-in receiver + sender, no wiring needed |
| **Channel** | Auto-aligned to the same WiFi channel as the Central Hub |

### Critical Configuration Note:
> **All devices auto-align to the same WiFi channel.** Room scanners connect to your WiFi first, which sets their radio to the same channel as the router (and the Central Hub). The wristband uses `WIFI_AP_STA` mode with a soft AP on the same channel, ensuring ESP-NOW broadcasts reach all scanners. No manual channel configuration is needed.

### Reliability Note:
> **Room scanners send each RSSI report 3 times** with 50ms gaps to combat the Central Hub's brief "deaf periods" during Firebase HTTP uploads. This ensures the Hub receives at least one copy even if it's mid-upload when the packet arrives.

### Setup Steps (Repeat for each of the 4 rooms):

Each room has its own pre-configured firmware file under `Room_Beacons/`:

| Room | Firmware File | Board Type |
|---|---|---|
| Living Room | `Living_Room/Living_Room.ino` | ESP32C3 Dev Module |
| Dining Room | `Dining_Room/Dining_Room.ino` | ESP32C3 Dev Module |
| Bed Room | `Bed_Room/Bed_Room.ino` | ESP32C3 Dev Module |
| Kitchen | `Kitchen/Kitchen.ino` | ESP32C3 Dev Module |

1. Open the corresponding `.ino` file for the room.
2. Paste the Hub's MAC address into `HUB_MAC` in the local `secrets.h`.
3. Update `WIFI_SSID` and `WIFI_PASSWORD` in the local `secrets.h` to match the bench network.
4. Flash to the ESP32-C3 using board type **ESP32C3 Dev Module**.
5. Plug it into the wall in the corresponding room.
6. The scanner will automatically reconnect to WiFi if the connection drops.

---

## Complete Wiring Diagram (Wristband ESP32)

```
                     ┌──────────────────────────┐
                     │         ESP32             │
                     │                          │
    LiPo(+)─────────┤  SIM800L VCC             │
    LiPo(-)─┬───────┤  SIM800L GND             │
             │       │                          │
             └───────┤  GND                     │
                     │                          │
    SIM800L TX ──────┤  GPIO 18 (RX)            │
    SIM800L RX ──────┤  GPIO 19 (TX)            │
                     │                          │
    NEO-6M TX  ──────┤  GPIO 4  (RX)            │
    NEO-6M RX  ──────┤  GPIO 5  (TX)            │
                     │                          │
    DHT11 DATA ──────┤  GPIO 15                 │
                     │                          │
    MPU-6050 SDA ────┤  GPIO 25 (I2C Bus 0 SDA) │
    MPU-6050 SCL ────┤  GPIO 26 (I2C Bus 0 SCL) │
                     │                          │
    MAX30102 SDA ────┤  GPIO 22 (I2C Bus 1 SDA) │
    MAX30102 SCL ────┤  GPIO 21 (I2C Bus 1 SCL) │
                     │                          │
    SOS Button ──────┤  GPIO 35 (INPUT, 10K↑)   │
                     │                          │
    All VCC ─────────┤  3V3                     │
    All GND ─────────┤  GND                     │
                     └──────────────────────────┘
```

---

## Assembly Tips
- **Dual I2C Buses:** The MPU-6050 (GPIO 25/26) and MAX30102 (GPIO 22/21) are on separate I2C buses. Keep wires for BOTH buses as short as possible (under 15cm) to prevent signal noise.
- **SOS Button Resistor:** Don't forget the external 10K pull-up resistor between GPIO 35 and 3V3 — without it, the button reads will be unreliable.
- **Antennas:** Ensure the SIM800L spring antenna and the NEO-6M ceramic antenna face outward and are NOT blocked by metal.
- **Shared Ground:** The LiPo battery GND and the ESP32 GND **must** be connected together, or the SIM800L serial communication will fail.
- **WiFi Channel Auto-Alignment:** All devices auto-align to the same WiFi channel. The wristband uses `WIFI_AP_STA` mode, and room scanners connect to WiFi first — this ensures ESP-NOW packets reach all devices on the same channel. No manual channel configuration is needed.
- **Triple-Send Reliability:** Room scanners forward each RSSI report 3 times with 50ms gaps to combat the Hub's brief deaf periods during Firebase HTTP uploads.
- **GPS Upload Behavior:** GPS coordinates are uploaded to Firebase whenever they are valid (outdoors). Indoors, the Central Hub manages `currentRoom` via ESP-NOW RSSI triangulation.
- **Room Scanner Placement:** Place each ESP32-C3 near the center of its room for the most accurate RSSI readings.
- **Hub Placement:** Place the Central Hub near your Wi-Fi router for the strongest internet connection.
- **Outdoors Debounce:** The Hub requires 2 consecutive evaluation cycles (10 seconds total) with all RSSI values below -90 dBm before declaring "Outdoors", preventing brief signal dips from false-triggering.
