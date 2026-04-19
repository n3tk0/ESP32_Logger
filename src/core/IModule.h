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
//   • schema()              — PROGMEM JSON that drives Form.bind() in the UI
//
// Phase 1 only ships the interface + ModuleRegistry with no modules wrapped.
// Existing setupXxx() functions keep working unchanged.  Subsequent phases
// wrap managers one by one; see Audit_report_17042026.md §5.8.
// ============================================================================
class IModule {
public:
    virtual ~IModule() = default;

    // Stable id used in modules.json keys and /api/modules/:id URLs.
    // Must be [a-z0-9._-]+ and survive across firmware versions.
    virtual const char* getId()   const = 0;

    // Human-readable display name for the settings tab strip.
    virtual const char* getName() const = 0;

    // ------------------------------------------------------------------
    // Config round-trip
    //   load(): merge fields from `cfg` into this module's in-memory state.
    //           Called at boot with the module's slice of modules.json and
    //           at runtime when POST /api/modules/:id arrives.
    //           Return false if validation rejects the payload.
    //   save(): write this module's current state into `cfg`.
    //           Called by the registry when persisting modules.json.
    // ------------------------------------------------------------------
    virtual bool load(JsonObjectConst cfg) = 0;
    virtual void save(JsonObject cfg) const = 0;

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

    // True if this module exposes a form to the UI.  When false the tab
    // is still listed but shows only an enable/disable switch.
    virtual bool hasUI() const          { return schema() != nullptr; }

    // JSON schema string (PROGMEM) that drives Form.bind().
    // Return nullptr to indicate "no form — toggle only".
    //
    // Schema shape (see audit §5.4):
    //   { "fields":[
    //       {"id":"ntpServer","type":"string","max":64,"label":"NTP"},
    //       {"id":"timezone","type":"int","min":-12,"max":14},
    //       {"id":"useStaticIP","type":"bool"},
    //       {"id":"staticIP","type":"ipv4","showIf":"useStaticIP"}
    //   ]}
    virtual const char* schema() const  { return nullptr; }

protected:
    bool _enabled = true;
};
