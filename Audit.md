# Firmware Audit — Water_logger v5.0 Architecture Review

## 🔴 Critical Issues (must fix)

* **Hybrid mode can enter deep sleep with FreeRTOS tasks still alive, so writes/exports can be cut mid-flight and task-owned buffers are never drained.** The v5 pipeline is started in continuous/hybrid mode from `setup()`, but the legacy loop still drops into `_doSleep()` from the idle/done states, and `TaskManager::shutdown()` is never called anywhere before restart or sleep. On the XIAO ESP32-C3 this is a real data-loss path, not a theoretical one.
  * **Why dangerous:** queued readings can be lost, `JsonLogger` may not flush its buffered lines, and in-flight exports are abandoned.
  * **Exact fix:** gate deep sleep off when `platformMode == hybrid`, or add a coordinated shutdown path that stops `SensorTask`, drains `storageQueue`/`exportQueue`, flushes `JsonLogger`, and only then sleeps/restarts.

* **The “non-blocking pipeline” is not actually non-blocking because sensor drivers block inside the highest-priority task.** `SensorTask` calls `sensorManager.tick()` in a tight loop, and `tick()` directly invokes each plugin’s `readAll()`. Several plugins then block for milliseconds to seconds: `WindSensor` sleeps for the full sample window, `SDS011` busy-waits up to 1.5 s, and `PMS5003` up to 2 s.
  * **Why dangerous:** one slow sensor stalls all other sensors, delays queue production, and on a unicore target competes with the rest of the system on the same CPU.
  * **Exact fix:** move each blocking sensor to its own task/state machine, or redesign plugin reads as non-blocking stateful pollers that return immediately.

* **Queue overflow causes silent data loss at every stage, and the export path can self-induce backlog for tens of seconds.** `SensorManager` drops when `sensorQueue` is full, `ProcessingTask` ignores failure when forwarding to storage, and export forwarding is explicitly non-blocking with no retry queue or drop counter. Then `ExportManager` can block `ExportTask` for exponential backoff delays while it retries sequentially.
  * **Why dangerous:** under bad Wi‑Fi or slow storage, the system will silently lose measurements with no operator visibility.
  * **Exact fix:** check all queue return codes, add per-queue drop counters/metrics, separate retry storage from live ingest, and bound exporter retry latency with a circuit breaker rather than sleeping inside the consumer.

* **`/api/data` can return incomplete or logically incorrect history.** The handler reads from the in-memory ring first and only queries the filesystem if the ring returns zero rows; if the ring has only a partial tail of the requested interval, older history is omitted entirely. Separately, `JsonLogger::query()` walks files in filesystem iteration order and never sorts by timestamp, while `AggregationEngine` assumes ordered time-series input.
  * **Why dangerous:** charts and downstream consumers can see missing early samples, wrong bucket grouping, and invalid LTTB downsampling.
  * **Exact fix:** always merge ring-buffer tail with filesystem history, de-duplicate by `(ts,id,metric)`, and sort before bucketing/LTTB.

* **The advertised SD + LittleFS hybrid storage path is effectively not wired into the running pipeline.** The pipeline is started with `activeFS`, `StorageTask` writes a single `JsonLogger` to that one FS, and `HybridStorage::begin()/mirrorWrite()` are never used.
  * **Why dangerous:** the documented “SD primary + LittleFS fallback/cache” behavior does not actually happen in the main data path.
  * **Exact fix:** initialize `HybridStorage` during startup, pass `HybridStorage::primary()` into storage, and use `mirrorWrite()` or an explicit dual-write strategy from `JsonLogger`.

* **Hybrid mode risks ISR/resource conflict on the flow sensor.** Legacy mode always attaches `onFlowPulse()` on the configured flow pin, while the default v5 config also enables `flow_main` (`yfs201`) on pin 21, and that plugin installs its own ISR on the same GPIO.
  * **Why dangerous:** this is a fragile overlap between two ownership models for the same hardware signal and can lead to ISR conflicts or undefined pulse accounting.
  * **Exact fix:** make hybrid mode explicitly share one pulse source abstraction, or forbid registering `yfs201` on the same pin used by legacy `pinFlowSensor`.

## 🟠 Major Improvements

* **Your default aggregation path destroys short spikes before LTTB ever sees them.** `AGG_LTTB` first buckets using AVG, then applies LTTB only after that. With the default config/UI of `5m + lttb`, transient rain/flow/wind spikes are flattened inside the 5-minute bucket.
  * **Fix:** default to raw+LTTB for short windows, or add min/max envelope aggregation for spiky metrics.

* **Storage config in `platform_config.json` is mostly ignored by the actual logger.** The file exposes `log_dir`, `rotate_daily`, and `max_file_size_kb`, but `StorageTask` calls `logger.begin(*fs)` with defaults and `ApiHandlers` queries a `JsonLogger` that is never initialized from config.
  * **Fix:** parse storage config once at startup/reload and inject it into both the write-side and read-side logger instances.

* **Power-loss behavior is weak for JSONL persistence.** The logger buffers 8 lines in RAM before flush, writes directly into the target file, and closes only after the batch.
  * **Fix:** flush on every critical reading for water volume totals, or write to a `.part`/journal buffer with newline+commit semantics.

* **File rotation/day partitioning is wrong across midnight boundaries.** `JsonLogger::flush()` chooses the file date from only the first buffered line, then writes the whole batch there.
  * **Fix:** split buffered lines by derived date before append, or flush immediately whenever the date key changes.

* **Export configuration promises features that the implementation does not honor.** MQTT exposes `qos` in config but publish is still QoS 0 only; HTTP/MQTT configs expose `interval_ms`, but those exporters send whenever `ExportTask` flushes the batch.
  * **Fix:** remove unsupported knobs or implement an export scheduler per exporter.

* **API semantics are inconsistent for multi-metric data.** `/api/data` allows `sensor` without `metric`, but it returns only `{ts,v}` with no metric/unit tags, so multi-metric sensors can produce a mixed, unitless series.
  * **Fix:** require `metric` for scalar series responses, or return grouped series per metric.

* **Sensor status reporting is misleading because `last_read_ts` is never updated.** The UI renders a “Last” timestamp, but every plugin resets `_lastReadTs` to `0` and `SensorManager` never sets it after successful reads.
  * **Fix:** update plugin state centrally in `SensorManager::tick()` after a successful `readAll()`.

* **Bus ownership is too implicit for a modular platform.** Multiple I2C plugins all call `Wire.begin(...)` independently, and both UART PM plugins hard-bind to `Serial2`.
  * **Fix:** introduce a bus manager that owns `Wire`/UART instances, validates pin collisions, and hands out shared bus handles to plugins.

## 🟡 Minor Improvements

* **`ExportManager::loadAndInit()` always returns success once JSON parses because it returns `ok >= 0`.**
  * **Fix:** return `ok > 0` or return a richer status enum.

* **`if (!sensor["enabled"] | false)` is a logic smell.**
  * **Fix:** replace with `if (!(sensor["enabled"] | false))`.

* **The ring buffer is documented as “lock-free” but is protected externally by a mutex and is not dual-core safe as written.**
  * **Fix:** either make it actually lock-free with atomics or change the contract and comments.

* **The Core Logic / Export pages expose restart-and-reload behavior differently from the new API handler.**
  * **Fix:** pick one model and remove the dead path.

* **`MAX_RAW = 2000` is a hidden server-side truncation point.**
  * **Fix:** return a `truncated:true` flag or paginate.

* **You have no runtime observability for stack headroom, queue high-water marks, or dropped samples.**
  * **Fix:** expose diagnostics counters and FreeRTOS high-water marks.

## 🟢 Strengths

* **The new v5 code is at least conceptually segmented in the right direction.** Sensors, pipeline, tasks, storage, export, and web/API each have their own modules instead of being collapsed into the old monolith.

* **`SensorReading` is compact, fixed-field, and stable enough for queue transport and JSONL logging.** That is the right direction for embedded determinism compared with a fully dynamic schema.

* **The exporter/plugin abstractions are simple and easy to extend.** `ISensor` and `IExporter` are minimal enough that adding a new sensor or target does not require invasive changes across the codebase.

* **You correctly preserve raw sensor data and mark bad readings with quality metadata instead of silently discarding them before storage.**

* **Legacy water-logger logic is still mostly isolated in the original manager/main-loop path instead of being fully rewritten around RTOS.**

## 🔧 Refactor Plan (step-by-step)

1. **Stabilize lifecycle first.** Introduce an explicit platform mode controller that owns startup/shutdown of the v5 pipeline, and make deep sleep illegal until `TaskManager::shutdown()` has drained storage/export.
2. **Separate blocking sensor I/O from the central poller.** Convert UART and pulse-window sensors into individual workers or non-blocking state machines.
3. **Add backpressure accounting.** Every queue send/receive path needs counters for dropped, retried, and drained items, plus a diagnostics endpoint.
4. **Make storage configuration real.** Parse `platform_config.json` once into a storage runtime struct, initialize `JsonLogger` with those values, and actually route through `HybridStorage`.
5. **Fix historical query correctness.** Rework `/api/data` so it always queries the filesystem for the missing range, merges ring-buffer tail, sorts by timestamp, then aggregates.
6. **Tighten API contracts.** Require `metric` for scalar chart responses, remove dead config endpoints, and drop unsupported config fields from the UI until implemented.
7. **Introduce bus/resource arbitration.** Centralize I2C/UART/pulse resource ownership and collision checks so plugin init cannot silently stomp shared hardware configuration.
8. **Only after the above, optimize memory/perf.** Stream aggregation from files, reduce heap churn in `/api/data`, and add stack watermark telemetry per task.

## 🚀 Performance Optimization Ideas

* **Stream aggregation directly from JSONL instead of materializing `MAX_RAW` readings first.** For AVG/MIN/MAX/SUM, aggregate per bucket while reading the file line-by-line and avoid the raw array entirely.

* **Replace `String body` in `/api/data` with `AsyncResponseStream` or chunked JSON writing.** Right now large responses still build one contiguous heap object despite the comment claiming otherwise.

* **Pre-index log files by day and skip irrelevant files by filename before parsing lines.** Your filenames are already day-based; exploit that to avoid scanning every `.jsonl` for narrow queries.

* **Use metric-specific aggregation defaults.** Example: `flow_rate` and `rain_rate` should prefer raw/LTTB or min-max envelope, while `temperature` is fine with AVG buckets.

* **Persist exporter retry state outside the live export queue.** A small per-exporter retry mailbox or LittleFS spool file will prevent bad network conditions from stalling the main export consumer.

* **Add compile-time or runtime queue sizing by platform mode/sensor count.** `QUEUE_SENSOR_DEPTH 20` is too static for a configurable multi-sensor platform; derive it from enabled sensors and worst-case burst fan-out.
