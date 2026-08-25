// ============================================================================
// node/src/node_config.h — everything you need to change for your deployment.
//
// Keep real credentials out of git. Either edit this file and leave it
// untracked, or override each value from platformio.ini:
//
//   build_flags =
//       -DWIFI_SSID='"my-network"'
//       -DWIFI_PASS='"my-password"'
//       -DCOLLECTOR_HOST='"192.168.1.50"'
//       -DINGEST_TOKEN='"the-same-token-as-the-collector"'
// ============================================================================
#pragma once

// ── WiFi ────────────────────────────────────────────────────────────────────
#ifndef WIFI_SSID
#  define WIFI_SSID   "your-ssid"
#endif
#ifndef WIFI_PASS
#  define WIFI_PASS   "your-password"
#endif

// ── Collector (the ESP32-C3) ────────────────────────────────────────────────
// An IP address, not an mDNS name: the ESP8266 would need a second library to
// resolve one, and a DHCP reservation on the router is the more reliable fix.
#ifndef COLLECTOR_HOST
#  define COLLECTOR_HOST "192.168.1.50"
#endif
#ifndef COLLECTOR_PORT
#  define COLLECTOR_PORT 80
#endif

// Must match INGEST_TOKEN on the collector, or every POST comes back 401.
#ifndef INGEST_TOKEN
#  define INGEST_TOKEN "change-me"
#endif

// Set both if the collector was built with WEB_BASIC_AUTH_ENABLED. Leaving
// the user empty skips the Authorization header entirely.
#ifndef COLLECTOR_BASIC_USER
#  define COLLECTOR_BASIC_USER ""
#endif
#ifndef COLLECTOR_BASIC_PASS
#  define COLLECTOR_BASIC_PASS ""
#endif

// ── Identity ────────────────────────────────────────────────────────────────
// The name this node posts under. The collector's remote sensor config must
// name the same string in its "node" field.
#ifndef NODE_ID
#  define NODE_ID "balcony"
#endif

// ── Timing ──────────────────────────────────────────────────────────────────
// How often to sample and post. Outdoor air does not move fast; 60 s is
// plenty and keeps the collector's ingest table quiet.
#ifndef POST_INTERVAL_MS
#  define POST_INTERVAL_MS 60000UL
#endif

// Give up on a WiFi association attempt after this long and retry later,
// rather than blocking the loop forever behind an AP that is not coming back.
#ifndef WIFI_CONNECT_TIMEOUT_MS
#  define WIFI_CONNECT_TIMEOUT_MS 20000UL
#endif

// ── Sensor ──────────────────────────────────────────────────────────────────
// NodeMCU V3 silkscreen → GPIO:  D1 = GPIO5, D2 = GPIO4.
// The usual BMx280 breakout wiring is SCL→D1, SDA→D2.
#ifndef I2C_SDA_PIN
#  define I2C_SDA_PIN 4
#endif
#ifndef I2C_SCL_PIN
#  define I2C_SCL_PIN 5
#endif

// BME280/BMP280 breakouts ship as either 0x76 or 0x77 depending on how SDO is
// strapped. The node probes both, starting here.
#ifndef BMX280_ADDR
#  define BMX280_ADDR 0x76
#endif

// Height of the sensor above sea level, in metres. Used only to report
// pressure at sea level alongside the raw station pressure — a forecast
// service quotes sea-level pressure, so comparing the two needs this.
// Leave at 0 to publish station pressure only.
#ifndef ALTITUDE_M
#  define ALTITUDE_M 0.0f
#endif
