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
//#ifndef NODE_SENSOR_BH1750
//#  define NODE_SENSOR_BH1750        // BH1750 ambient light (I2C) -> lux
//#endif
//#ifndef NODE_SENSOR_SDS011
//#  define NODE_SENSOR_SDS011        // SDS011 particulate (UART) -> pm25, pm10
//#endif
//#ifndef NODE_SENSOR_PULSE
//#  define NODE_SENSOR_PULSE         // Reed/hall pulse input -> rain or water flow
//#endif

// BMX280 and BME688 do the same job and publish the same metric names, so
// enabling both would have them overwrite each other in the collector's
// ingest table, which is keyed by (node, metric). They are alternatives.
#if defined(NODE_SENSOR_BMX280) && defined(NODE_SENSOR_BME688)
#  error "Enable NODE_SENSOR_BMX280 or NODE_SENSOR_BME688, not both — they publish the same metrics"
#endif

// ── Metric budget ───────────────────────────────────────────────────────────
// The collector drains a remote node through the ordinary plugin path, and
// SensorManager hands every plugin a fixed array of 8 readings per tick
// (MAX_METRICS_PER_TICK). A node publishing more than that is not an error
// anywhere — the surplus is simply not copied, which is exactly the kind of
// silent loss that is painful to diagnose from the dashboard end.
//
// So count what this build will emit and say so at compile time. How many
// DS18B20 probes are on the wire cannot be known here; set
// NODE_DS18B20_EXPECTED if you run more than one.
#ifndef NODE_DS18B20_EXPECTED
#  define NODE_DS18B20_EXPECTED 1
#endif

// Per-sensor metric counts. Spelled out with #ifdef rather than defined()
// inside the sum: using defined() in a macro body is undefined behaviour and
// GCC warns about it (-Wexpansion-to-defined).
#ifdef NODE_SENSOR_BMX280
#  define NODE_MC_BMX280  4      // temperature, humidity, pressure, pressure_sea
#else
#  define NODE_MC_BMX280  0
#endif
#ifdef NODE_SENSOR_BME688
#  define NODE_MC_BME688  5      // the above plus gas_resistance
#else
#  define NODE_MC_BME688  0
#endif
#ifdef NODE_SENSOR_BH1750
#  define NODE_MC_BH1750  1      // lux
#else
#  define NODE_MC_BH1750  0
#endif
#ifdef NODE_SENSOR_SDS011
#  define NODE_MC_SDS011  2      // pm25, pm10
#else
#  define NODE_MC_SDS011  0
#endif
#ifdef NODE_SENSOR_PULSE
#  define NODE_MC_PULSE   2      // rate, total
#else
#  define NODE_MC_PULSE   0
#endif
#ifdef NODE_SENSOR_DS18B20
#  define NODE_MC_DS18B20 NODE_DS18B20_EXPECTED
#else
#  define NODE_MC_DS18B20 0
#endif

#define NODE_METRIC_COUNT (NODE_MC_BMX280 + NODE_MC_BME688 + NODE_MC_BH1750 + \
                           NODE_MC_SDS011 + NODE_MC_PULSE  + NODE_MC_DS18B20)

#if NODE_METRIC_COUNT > 8
#  warning "This sensor set emits more than 8 metrics; the collector copies only the first 8 per tick and drops the rest silently. Split the sensors across two nodes, or trim the set."
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

// ── BH1750 (light) ──────────────────────────────────────────────────────────
// 0x23 with ADDR low (the usual breakout), 0x5C with ADDR high. Shares the
// I2C pins above.
#ifndef BH1750_ADDR
#  define BH1750_ADDR 0x23
#endif

// ── SDS011 (particulate) ────────────────────────────────────────────────────
// The ESP8266's only hardware UART is the console, so the SDS011 is read over
// SoftwareSerial. 9600 baud is slow enough for that to be reliable, but the
// pins must be interrupt-capable: GPIO16 cannot be used for RX.
//
// D5 = GPIO14, D7 = GPIO13. Wire the SDS011's TXD to RX_PIN.
// TX_PIN is only needed to put the sensor into duty-cycle mode; leave it as
// is and the node just listens to the 1 Hz stream the sensor sends by default.
#ifndef SDS011_RX_PIN
#  define SDS011_RX_PIN 14
#endif
#ifndef SDS011_TX_PIN
#  define SDS011_TX_PIN 13
#endif

// ── Pulse input (rain gauge / water flow) ───────────────────────────────────
// One counter serving both jobs: a tipping-bucket reed switch and a hall-
// effect flow sensor differ only in scale and in what the numbers are called.
//
// GPIO4 (D2) is free of boot straps and of the pins the other sensors default
// to. It previously defaulted to 13, which is SDS011_TX_PIN — SoftwareSerial
// claimed the pin as TX and then attachInterrupt() was layered on top of it.
// That combination compiles cleanly and only fails on hardware, which is the
// worst place to find it, so the defaults are now distinct by construction:
//
//   4  pulse        5  I2C SCL (and 4 is I2C SDA — see below)
//   12 1-Wire      13  SDS011 TX      14 SDS011 RX
//
// Note 4 is also the I2C SDA default: a build with BOTH an I2C sensor and the
// pulse input still needs one of them moved in the portal. There is no
// assignment that avoids every clash on eleven usable GPIOs — the point is
// that the common single-extra-sensor cases now work untouched.
#ifndef PULSE_PIN
#  define PULSE_PIN 4
#endif

// "rain" -> rain_rate (mm/h) + rain_total (mm)
// "flow" -> flow_rate (L/min) + flow_total (L)
#ifndef PULSE_MODE_RAIN
#  define PULSE_MODE_RAIN 1         // set to 0 for flow
#endif

// How much of the measured quantity one pulse represents.
//   rain: mm per bucket tip. 0.2794 = the common 0.011" tipping bucket.
//   flow: litres per pulse. A YF-S201 is ~450 pulses/litre -> 0.00222.
#ifndef PULSE_UNITS_PER_PULSE
#  if PULSE_MODE_RAIN
#    define PULSE_UNITS_PER_PULSE 0.2794f
#  else
#    define PULSE_UNITS_PER_PULSE 0.00222f
#  endif
#endif

// Minimum microseconds between accepted pulses, to reject contact bounce.
// A reed switch on a rain gauge bounces for a few ms and tips at most a few
// times a second, so 10 ms is generous. A hall flow sensor can legitimately
// produce hundreds of pulses a second, and any debounce large enough to help
// a reed switch would silently cap the flow reading — so it defaults to 0.
#ifndef PULSE_DEBOUNCE_US
#  if PULSE_MODE_RAIN
#    define PULSE_DEBOUNCE_US 10000UL
#  else
#    define PULSE_DEBOUNCE_US 0UL
#  endif
#endif

// Height of the sensor above sea level, in metres. Used only to report
// pressure at sea level alongside the raw station pressure — a forecast
// service quotes sea-level pressure, so comparing the two needs this.
// Leave at 0 to publish station pressure only.
#ifndef ALTITUDE_M
#  define ALTITUDE_M 0.0f
#endif
