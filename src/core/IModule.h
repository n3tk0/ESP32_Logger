#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ============================================================================
// IModule — unified module interface (Pass 5, phase 1).
//
// Generalises the ISensor pattern to cover every runtime subsystem that has
// persisted configuration and/or a UI form: wifi, ota, theme, datalog, time,
// export.mqtt, export.webhook, …  A module owns:
//
//   • a stable string id    — used in /api/modules/:id and modules.json key
//   • a human-readable name — shown in the tab strip
//   • load()/save()         — round-trip JsonObject ↔ in-memory state
//   • start()/stop()        — optional hot-start lifecycle (else flag restart)
//   • schema()              — gzipped JSON that drives the form in the UI
//
// Phase 1 only ships the interface + ModuleRegistry with no modules wrapped.
// Existing setupXxx() functions keep working unchanged.  Subsequent phases
// wrap managers one by one; see Audit_report_17042026.md §5.8.
// ============================================================================
// A module's form schema: a gzip stream in flash (see schema() below).
struct ModuleSchema {
    const uint8_t* gz;    // nullptr: the module has no form
    size_t         len;
};

class IModule {
public:
    virtual ~IModule() = default;

    // Stable id used in modules.json keys and /api/modules/:id URLs.
    // Must be [a-z0-9._-]+ and survive across firmware versions.
    virtual const char* getId()   const = 0;

    // Human-readable display name for the settings tab strip.
    virtual const char* getName() const = 0;

    // Optional one-line description shown under the module name in the manager.
    // Keep it short (≤ ~80 chars). Default "" means "no subtitle".
    virtual const char* getDescription() const { return ""; }

    // ------------------------------------------------------------------
    // Config round-trip
    //   load(): merge fields from `cfg` into this module's in-memory state.
    //           Called at boot with the module's slice of modules.json and
    //           at runtime when POST /api/modules/:id arrives.
    //           Return false if validation rejects the payload.
    //
    //           A FALSE RETURN DOES NOT PROMISE NOTHING CHANGED, and callers
    //           must not assume it does. Every module written so far assigns
    //           as it parses and reports the failure at the end, so a rejected
    //           payload has usually been partly applied. POST /api/modules/:id
    //           compensates by snapshotting DeviceConfig and putting it back —
    //           which covers the config-backed modules and cannot cover a
    //           module's own members. A new module that can reject a payload
    //           should therefore validate everything BEFORE it assigns
    //           anything; then the compensation is redundant rather than
    //           load-bearing.
    //   save(): write this module's current state into `cfg`.
    //           Called by the registry when persisting modules.json.
    // ------------------------------------------------------------------
    virtual bool load(JsonObjectConst cfg) = 0;
    virtual bool save(JsonObject cfg) const = 0;

    // ------------------------------------------------------------------
    // Optional lifecycle hooks.  Default no-op keeps phase-1 wrappers tiny.
    //   start() — bring the module online with current config.
    //             Return false if a reboot is required to apply.
    //   stop()  — release resources (e.g. wifi down, task delete).
    //   tick()  — called from the module task once per loop if present.
    // ------------------------------------------------------------------
    virtual bool start()                { return true; }
    virtual void stop()                 {}
    virtual void tick(uint32_t nowMs)   { (void)nowMs; }

    // Runtime enable/disable (web UI toggle, no reboot required when the
    // module reports start() == true after re-enable).
    virtual bool isEnabled() const      { return _enabled; }
    virtual void setEnabled(bool e)     { _enabled = e; }

    // Optional live status for the manager's status chip.  Populate `out`:
    //     out["text"] = "synced";          // short, human-readable
    //     out["tone"] = "ok"|"warn"|"err"|"dim";
    // Leave `out` empty to let the UI fall back to its enabled/disabled chip.
    // MUST be cheap and non-blocking — it runs on the AsyncTCP worker during
    // GET /api/modules (one call per module per request).  No FS scans, no
    // network round-trips.  Convention: return early when !isEnabled() so the
    // UI shows a plain "disabled" chip.
    virtual void statusJson(JsonObject out) const { (void)out; }

    // True if this module exposes a form to the UI.  When false the tab
    // is still listed but shows only an enable/disable switch.
    virtual bool hasUI() const          { return schema().gz != nullptr; }

    // The form's field list, gzipped, as GET /api/modules/:id/schema sends it
    // (Content-Encoding: gzip — the browser inflates it, the firmware never
    // does). Return {nullptr, 0} for "no form — toggle only".
    //
    // A module does not write these bytes. Its schema is a JSON file,
    // src/modules/schemas/<id>.json, which scripts/gen_module_schemas.py
    // checks and compresses into src/modules/ModuleSchemas.h; schema() returns
    // that array. Shape (see audit §5.4):
    //   { "fields":[
    //       {"id":"ntpServer","type":"string","max":64,"label":"NTP"},
    //       {"id":"timezone","type":"int","min":-12,"max":14},
    //       {"id":"useStaticIP","type":"bool"},
    //       {"id":"staticIP","type":"ipv4","showIf":"useStaticIP"}
    //   ]}
    virtual ModuleSchema schema() const { return {nullptr, 0}; }

protected:
    bool _enabled = true;
};
