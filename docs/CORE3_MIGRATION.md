# Arduino core 3.x — what the migration costs, measured

We build against **Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7**. That IDF branch
reached [end of life in July 2024][eol] — no further bug or security fixes. For
a device that runs a web server and accepts OTA over WiFi, that is the real
argument for moving, and it does not go away.

The question is when, and the answer turns on flash. This document records what
was measured rather than what was assumed, so the decision can be re-taken later
against numbers instead of impressions.

Reproduce any row with `pio run -e x_core3_probe` (see `platformio.ini`).

> **These figures predate `-fno-exceptions`.** They were taken with exceptions
> enabled, before that flag was added to `[env] build_flags`, and are left as
> measured rather than rewritten — every row was taken the same way, so the
> deltas between them (the point of the table) still hold. Absolute numbers on
> both sides are now ~137 KB smaller. The conclusion is unchanged and if
> anything firmer: the core still costs ~223 KB, and the headroom that
> `-fno-exceptions` just bought should not be spent on paying for it.

## The numbers

App flash on `xiao_esp32c3`, default `src/setup.h`, against the 1,507,328-byte
app partition (`app0`) of `partitions_balanced.csv`.

**App flash is what PlatformIO prints and checks — it is NOT what decides
whether a build fits.** That figure excludes `.eh_frame`, which is flashed
anyway; with exceptions enabled it understated the image by 88,892 bytes.
`firmware.bin` is the artifact that has to fit `app0`, and it is what the README
and the CI size gates use. The table below is in app flash only because that is
how it was measured at the time, and every row is measured the same way, so the
deltas — the point of the table — are unaffected. Do not read the "free" column
as headroom.

| build | app flash | of partition | free | RAM |
|---|---:|---:|---:|---:|
| core 2.0.17, esphome AsyncWebServer — **what we ship** | 1,382,104 | 91.7 % | 125,224 | 55,116 |
| core 2.0.17, ESP32Async AsyncWebServer | 1,386,716 | 92.0 % | 120,612 | 55,044 |
| core 3.3.11 / IDF 5.5.5, stock sdkconfig | 1,609,647 | **106.8 %** | **−102,319** | 52,612 |
| core 3.3.11, with the sdkconfig levers below | 1,484,222 | 98.5 % | 23,106 | 49,344 |

Read as deltas:

- **the web-server library swap: +4,612 bytes.** Small, and worth isolating —
  without this row the library change would have been silently attributed to
  the core.
- **the core itself: +222,931 bytes.** This is the whole story. It matches the
  +224 KB an ESP32-C3 user reported on 3.0.1 in [discussion #9860][d9860],
  where the maintainer's explanation was that the IDF WiFi stack "grew by about
  200KB because of support for newer encryption methods".
- **sdkconfig access: −125,425 bytes.** Real, and only available on core 3.x
  (see below).

**Net effect of migrating today: +102,118 bytes, and free space falls from
125,224 to 23,106.** The all-features build would not fit at all.
(That build was ~1,451,000 when this was measured; `-fno-exceptions` has since
taken it to ~1,397,000, which does not change the comparison — both sides of
the table moved by the same amount.)

RAM improves slightly in every step. It was never the constraint.

## Why sdkconfig is only reachable on core 3.x

The official `platformio/espressif32` platform pins
`framework-arduinoespressif32 ~3.20017.0` and links **prebuilt** ESP-IDF
archives. `sdkconfig` appears nowhere in its Arduino build path — only under
`framework = espidf` (verified in `builder/frameworks/espidf.py`). Editing
`sdkconfig` in the project tree does nothing, which the Arduino-ESP32 FAQ also
states.

[pioarduino/platform-espressif32][pioarduino] adds `custom_sdkconfig` for
Arduino builds and rebuilds ESP-IDF from source to honour it. It is core 3.x
only. That is the whole reason the two questions — "should we move to 3.x" and
"can we shrink the WiFi stack" — are the same question.

## What the levers actually gave, and what refused

Working (in `[env:x_core3_probe]`, −125,425 bytes together):

```
CONFIG_ESP_WIFI_FTM_ENABLE=n            802.11mc ranging, unused
CONFIG_ESP_WIFI_CSI_ENABLED=n           channel state info, unused
CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n       WPA3; sae.c alone is 10,540 bytes here
CONFIG_ESP_WIFI_ENABLE_SAE_PK=n
CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=n
CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n    802.1X, unused
CONFIG_MBEDTLS_ERROR_STRINGS=n          15,674 bytes in our 2.0.17 map
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y
```

Two documented levers did **not** work, and both failures are structural rather
than typos:

- **`CONFIG_LWIP_IPV6=n`** — the biggest single lever, [documented at ~39 KB][lwip].
  It does not compile. Arduino's `idf_component.yml` pulls in managed components
  this project never uses, and `espressif__esp-modbus` is not IPv6-optional:
  `lwipopts.h` expands `LWIP_IPV6_NUM_ADDRESSES` to an undeclared `CONFIG_`
  symbol, and it calls `mdns_query_aaaa()` unguarded. Getting the 39 KB means
  stubbing out every unused managed component first — which is exactly what
  [ESPHome did][esphome] to cut their Arduino build by 44 %.

- **`CONFIG_LIBC_NEWLIB_NANO_FORMAT=y`** — documented at 25–50 KB. It does not
  link. pioarduino's hybrid mode rebuilds ESP-IDF from source but still links
  Arduino's **prebuilt** libraries, and that `libnewlib.a` keeps a stub-table
  reference to `_printf_float` / `_scanf_float` which nano format does not
  provide. Getting it needs the Arduino libraries rebuilt too, via
  [esp32-arduino-lib-builder][libbuilder].

Deliberately left ON: **`CONFIG_ESP_WIFI_SOFTAP_SUPPORT`**. The failsafe
recovery path is an AP the device raises when it cannot join a network.
Disabling soft-AP would save flash by deleting the way back in.

So the remaining headroom on core 3.x is real — plausibly another 60–90 KB
between IPv6 and nano format — but it is gated behind rebuilding the Arduino
libraries and stubbing managed components, not behind more lines in
`platformio.ini`.

## What the migration costs in code

Almost nothing, which was the surprise.

Everything the [2.x → 3.0 migration guide][migration] lists as breaking was
either already handled or never used:

| breaking change | us |
|---|---|
| LEDC (`ledcSetup`/`ledcAttachPin`/`ledcDetachPin` removed) | already dual-pathed — `HeaterModule.cpp` has three `#if ESP_ARDUINO_VERSION_MAJOR >= 3` blocks |
| Timer API (14 functions removed) | not used |
| RMT, I2S, SigmaDelta, touch, `hallRead`, DAC | not used |
| ADC `adcAttachPin` / `analogSetClockDiv` / `analogSetVRefPin` removed | not used; we use `analogSetPinAttenuation`, which survives |
| BLE `String` / `BLEScanResults*` | not used |
| WiFi `flush()` → `clear()`, `WiFiServer::available()` deprecated | not reached — we serve through AsyncWebServer |
| mbedTLS 2.28 → 3.x | we call `mbedtls_sha256_init/starts/update/finish/free`, which exist in both |

Our ESP-IDF surface (`esp_sleep_*`, `esp_ota_*`, `esp_random`,
`esp_reset_reason`, `esp_read_mac`, `esp_partition`, `esp_chip_info`,
`freertos/*`, `driver/gpio.h`) is stable across 4.4 → 5.5.

Two source changes were needed, both from the **library** swap rather than the
core, and both are in the tree now because they compile on 2.x as well:

1. `clientAcceptsGzip()` binds `getHeader()` to a `const AsyncWebHeader*` —
   ESP32Async returns const, the esphome fork does not, and the const pointer
   accepts both.
2. `AsyncWebHandler::canHandle()` is `const` in ESP32Async and non-const in the
   esphome fork. One signature cannot satisfy both, so `LOGGER_CANHANDLE_CV`
   (keyed off `ASYNCWEBSERVER_VERSION_MAJOR`, which only the ESP32Async line
   defines) supplies the qualifier.

## One trap worth knowing about

`platform = espressif32` resolves **by name**, and pioarduino's fork uses the
same name as the official platform. Installing it made PlatformIO select it for
every environment in `platformio.ini`, silently rebuilding the production
targets against a different Arduino core. Here it failed loudly — on a header
core 3.x removed — but a version that merely compiled would have been much
worse.

`[env]` now pins `platformio/espressif32@7.0.1` explicitly. Do not un-pin it.

Installing the fork also replaced the shared `tool-esptoolpy` package, whose
newer esptool needs the `intelhex` Python module. On a machine that has only
ever had the official platform this does not arise; on one that has built
`x_core3_probe`, `pip install intelhex` restores the official path.

## Conclusion

Do not migrate for flash — it costs about 100 KB net. Migrate when the EOL
security position forces it, and expect that when it does, the C3 with 4 MB and
two OTA slots will need either the deeper savings above (rebuilt Arduino
libraries, stubbed managed components) or a partition table without the second
OTA slot.

[eol]: https://documentation.espressif.com/AR2024-008%20End-of-Life%20Advisory%20for%20ESP-IDF%20v4.4%20Release%20Branch%20EN.html
[d9860]: https://github.com/espressif/arduino-esp32/discussions/9860
[migration]: https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html
[lwip]: https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-guides/lwip.html
[pioarduino]: https://github.com/pioarduino/platform-espressif32
[esphome]: https://github.com/esphome/esphome/pull/13623
[libbuilder]: https://docs.espressif.com/projects/arduino-esp32/en/latest/lib_builder.html
