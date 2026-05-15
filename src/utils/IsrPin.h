// ============================================================================
// src/utils/IsrPin.h
//
// RAII wrapper for ESP-IDF GPIO ISR registration.
//
// Solves the dangling-`this` use-after-free flagged in AUDIT 2.6, 23.2, 23.3:
// every sensor plugin that calls `gpio_isr_handler_add(pin, _isr, this)` must
// pair it with `gpio_isr_handler_remove(pin)` in its destructor; otherwise
// SensorManager::reloadConfig() deletes the plugin and the next IRQ fires
// the IRAM ISR on freed memory.
//
// Per REFACTORING_GUIDELINES Pillar 4.6, plugins MUST hold an `IsrPin` member
// AS THE LAST MEMBER OF THE CLASS so its destructor runs FIRST during delete
// (member destructors run in reverse declaration order).
//
// Usage:
//   class MyFlowSensor : public ISensor {
//   public:
//       bool init(...) override {
//           pinMode(_pin, INPUT_PULLUP);
//           return _isr.attach(_pin, GPIO_INTR_NEGEDGE, &MyFlowSensor::_isrFn,
//                              this);
//       }
//   private:
//       static void IRAM_ATTR _isrFn(void* arg);
//       uint8_t _pin = 21;
//       // ... other state ...
//       IsrPin  _isr;       // ★ LAST member — destructed first
//   };
//
// Move-only; copies are deleted to prevent double-detach.
// ============================================================================
#pragma once

#include <stdint.h>
#include <driver/gpio.h>

class IsrPin {
public:
    /// Default-constructed: not attached. Use attach() to bind.
    IsrPin() noexcept;
    ~IsrPin() noexcept;

    // ── Move-only ───────────────────────────────────────────────────────────
    IsrPin(const IsrPin&)            = delete;
    IsrPin& operator=(const IsrPin&) = delete;
    IsrPin(IsrPin&& other)            noexcept;
    IsrPin& operator=(IsrPin&& other) noexcept;

    /// Installs the ISR service (idempotent — only once per app) and
    /// registers `isr(arg)` on `pin` with the given interrupt type.
    ///
    /// Returns true on success. On failure, the IsrPin is left detached.
    /// Calling attach() on an already-attached pin detaches the previous
    /// binding first.
    bool attach(uint8_t         pin,
                gpio_int_type_t intrType,
                gpio_isr_t      isr,
                void*           arg) noexcept;

    /// Removes the registered handler. Safe to call when not attached.
    /// Called automatically by the destructor; expose for explicit reset.
    void detach() noexcept;

    bool   isAttached() const noexcept { return _attached; }
    uint8_t pin()       const noexcept { return _pin;       }

private:
    /// Ensure ESP-IDF's per-app GPIO ISR service is installed. Idempotent:
    /// safe to call from every attach(). On ESP32-C3 SuperMini (single
    /// core, CONFIG_FREERTOS_UNICORE) there is no race; on multi-core
    /// targets the underlying ESP-IDF call returns ESP_ERR_INVALID_STATE
    /// after the first installer wins, which we intentionally ignore.
    static void _ensureServiceInstalled() noexcept;

    uint8_t _pin;        // GPIO number, 0xFF when unattached
    bool    _attached;
};
