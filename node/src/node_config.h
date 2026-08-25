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

// ============================================================================
// SENSOR SELECTION — pick at build time; only what you pick is compiled in
// ============================================================================
// Same idea as the collector's setup.h toggles. Each one costs flash and RAM
// only when enabled, and the setup portal shows pin fields only for sensors
// that are actually in the build.
//
// All three drivers are the collector's own, included unmodified from
// ../src/drivers/ — so the compensation maths cannot drift between a wired
// sensor and a remote one.
// ----------------------------------------------------------------------------
#ifndef NODE_SENSOR_BMX280
#  define NODE_SENSOR_BMX280        // BME280 / BMP280 (I2C)
#endif
//#ifndef NODE_SENSOR_BME688
//#  define NODE_SENSOR_BME688        // BME680 / BME688 (I2C), adds gas resistance
//#endif
//#ifndef NODE_SENSOR_DS18B20
//#  define NODE_SENSOR_DS18B20       // DS18B20 (1-Wire), up to 8 on one pin
//#endif

// BMX280 and BME688 do the same job and publish the same metric names, so
// enabling both would have them overwrite each other in the collector's
// ingest table, which is keyed by (node, metric). They are alternatives.
#if defined(NODE_SENSOR_BMX280) && defined(NODE_SENSOR_BME688)
#  error "Enable NODE_SENSOR_BMX280 or NODE_SENSOR_BME688, not both — they publish the same metrics"
#endif

// ── I2C pins ────────────────────────────────────────────────────────────────
// NodeMCU V3 silkscreen → GPIO:  D1 = GPIO5, D2 = GPIO4.
// The usual BMx280 breakout wiring is SCL→D1, SDA→D2.
// These are DEFAULTS — the setup portal can override them per device.
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

// ── 1-Wire ──────────────────────────────────────────────────────────────────
// D3 = GPIO0 is the FLASH button and a boot strap; D4 = GPIO2 must be HIGH at
// reset. GPIO12 (D6) is free of both problems and needs only the usual 4.7 kΩ
// pull-up to 3V3. Also overridable from the portal.
#ifndef ONEWIRE_PIN
#  define ONEWIRE_PIN 12
#endif

// A DS18B20 alongside a BMx280/BME688 would collide on the metric name
// "temperature", and the collector's ingest table is keyed by (node, metric) —
// the second one to post would silently overwrite the first. So the 1-Wire
// probes publish under their own name by default: probe_temp, probe_temp_1,
// and so on.
//
// On a DS18B20-ONLY node there is nothing to collide with, and you may prefer
// the plain name so the series matches a wired DS18B20 elsewhere:
//   -DNODE_DS18B20_METRIC='"temperature"'
// Keep it at 10 characters or fewer: SensorReading::metric is 16 bytes and the
// multi-probe suffix needs the room.
#ifndef NODE_DS18B20_METRIC
#  define NODE_DS18B20_METRIC "probe_temp"
#endif

// Height of the sensor above sea level, in metres. Used only to report
// pressure at sea level alongside the raw station pressure — a forecast
// service quotes sea-level pressure, so comparing the two needs this.
// Leave at 0 to publish station pressure only.
#ifndef ALTITUDE_M
#  define ALTITUDE_M 0.0f
#endif
