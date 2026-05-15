// ============================================================================
// src/utils/IsrPin.cpp — see IsrPin.h for contract.
// ============================================================================
#include "IsrPin.h"

#include <esp_err.h>

// ---------------------------------------------------------------------------
namespace {
    // Process-lifetime flag — gpio_install_isr_service is idempotent inside
    // ESP-IDF (returns ESP_ERR_INVALID_STATE on the 2nd+ call) but doing the
    // check once here avoids that error path.
    volatile bool g_isrServiceInstalled = false;
}

void IsrPin::_ensureServiceInstalled() noexcept {
    if (g_isrServiceInstalled) return;

    // intr_alloc_flags = 0 → default (level 1, normal priority).
    // The ESP-IDF documentation calls this safe to run before the scheduler
    // is up. The "0 flags" path is what every plugin in this codebase used
    // before consolidation.
    esp_err_t err = gpio_install_isr_service(0);

    // ESP_OK on first call; ESP_ERR_INVALID_STATE if a prior call (e.g. by
    // some other library) already installed it. Either is acceptable.
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        g_isrServiceInstalled = true;
    }
}

// ---------------------------------------------------------------------------
IsrPin::IsrPin() noexcept
    : _pin(0xFF), _attached(false)
{
}

IsrPin::~IsrPin() noexcept {
    detach();
}

// ── Move ────────────────────────────────────────────────────────────────────
IsrPin::IsrPin(IsrPin&& other) noexcept
    : _pin(other._pin), _attached(other._attached)
{
    other._pin      = 0xFF;
    other._attached = false;
}

IsrPin& IsrPin::operator=(IsrPin&& other) noexcept {
    if (this != &other) {
        // Release whatever we currently own before adopting the source's.
        detach();
        _pin            = other._pin;
        _attached       = other._attached;
        other._pin      = 0xFF;
        other._attached = false;
    }
    return *this;
}

// ---------------------------------------------------------------------------
bool IsrPin::attach(uint8_t         pin,
                    gpio_int_type_t intrType,
                    gpio_isr_t      isr,
                    void*           arg) noexcept
{
    if (isr == nullptr) return false;
    if (pin >= GPIO_NUM_MAX) return false;

    // Re-attaching: drop the previous binding first.
    if (_attached) detach();

    _ensureServiceInstalled();

    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);

    // Configure interrupt edge/level. ESP-IDF accepts this both before and
    // after gpio_isr_handler_add — keep it before so a stray edge between
    // add and set_intr_type can't fire with the wrong polarity.
    if (gpio_set_intr_type(gpio, intrType) != ESP_OK) return false;

    if (gpio_isr_handler_add(gpio, isr, arg) != ESP_OK) return false;

    _pin      = pin;
    _attached = true;
    return true;
}

// ---------------------------------------------------------------------------
void IsrPin::detach() noexcept {
    if (!_attached) return;

    // Best-effort removal. If the pin number is invalid for any reason we
    // still flip _attached so the destructor doesn't loop trying.
    gpio_isr_handler_remove(static_cast<gpio_num_t>(_pin));

    _pin      = 0xFF;
    _attached = false;
}
