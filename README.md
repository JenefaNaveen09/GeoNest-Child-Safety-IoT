# GeoNest Child Safety IoT

GeoNest is an ESP32 research prototype with a sensor wristband, four indoor room
beacons, a central hub, and a React dashboard. It uses ESP-NOW for room reports,
Firebase Realtime Database for telemetry and geofences, and SIM800L for bench
testing of SMS alerts.

The wristband firmware uses acceleration thresholds for fall and impact alerts.
This repository covers the prototype. The 1D-CNN–BiLSTM–attention model, MHEALTH
preprocessing, training, and benchmark evaluation code are outside this release.
Use synthetic data and test devices when following the steps below.

## Dashboard setup

1. Create a separate Firebase demo project, register a web app, and create a
   Realtime Database in locked mode. Enable Email/Password in Firebase
   Authentication. Create a demo dashboard user. For hardware testing, create
   separate accounts for the hub and wristband.
2. Copy the account UIDs into a local copy of `database.rules.json` and apply
   those rules in the Realtime Database console. The dashboard account can read
   and write at `/demo-wristband`; the two device accounts can write there.
   Leave unused UID placeholders as-is. To populate a fresh database without
   hardware, import `demo-data.json` at the root of the empty demo database.
3. Copy `.env.example` to `.env` and enter the demo project's web app values.
   Enable the Maps JavaScript API and supply a browser key restricted to that API
   and your dashboard origin. Enter user credentials only in the sign-in form.
4. From this directory, run the following commands. Open
   [localhost:3000](http://localhost:3000) and sign in with the dashboard account.

   ```sh
   npm ci
   npm start
   ```

   Run `npm run build` to generate the dashboard bundle.

## ESP32 firmware setup

1. Follow the [wiring guide](hardware%20firmware%20code/Wiring_Guide.md).
   Use the Arduino ESP32 3.x board package. Install Adafruit MPU6050, Adafruit
   Unified Sensor, DHT sensor library, SparkFun MAX3010x, and TinyGPSPlus for
   the wristband.
2. In each sketch directory, copy `secrets.example.h` to `secrets.h`. Set
   the bench Wi-Fi credentials. Flash the hub first and use its station MAC
   for `HUB_MAC` in the wristband and room beacon configurations. Keep the
   devices on the same Wi-Fi channel for ESP-NOW.
3. In the hub and wristband configurations, set `FIREBASE_HOST` to the demo
   database HTTPS URL ending in `/demo-wristband.json`. Paste the trusted
   root CA PEM for that endpoint into `FIREBASE_ROOT_CA`. Allow NTP access
   so the ESP32 clock can synchronize for certificate verification.
4. For a short bench test, sign in as each device account through the Firebase
   Authentication REST API and use its ID token as `FIREBASE_AUTH` in the
   corresponding local header. The sketches use a manually provisioned token
   and require a fresh token after expiration. Leave the wristband phone field
   blank to disable SMS, or use a designated test number.

The `.env` and `secrets.h` files are ignored by Git. Keep credentials and
device-specific settings in these local files.

## Project files

- `src/` and `public/` contain the dashboard source and static assets.
- `hardware firmware code/` contains the ESP32 sketches and wiring guide.
- `database.rules.json` and `demo-data.json` contain the access rules template
  and synthetic starting data.
- [Prototype overview](docs/Prototype-Overview.md) describes the dashboard,
  telemetry fields, and hardware behavior.

## Associated manuscript

Jenefa Archpaul, T. M. Thiyagu, and Sheeba Merlin G.
“An Intelligent ESP32-Based Wearable IoT System for Real-Time Child Safety
Monitoring and Emergency Response.”
