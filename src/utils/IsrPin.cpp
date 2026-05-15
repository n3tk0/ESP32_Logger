// ============================================================================
// src/utils/IsrPin.cpp — see IsrPin.h for contract.
// ============================================================================
#include "IsrPin.h"

#include <atomic>
#include <esp_err.h>
#include <esp_intr_alloc.h>

// ---------------------------------------------------------------------------
namespace {
    // Process-lifetime flag — gpio_install_isr_service is idempotent inside
    // ESP-IDF (returns ESP_ERR_INVALID_STATE on the 2nd+ call) but doing the
    // check once here avoids that error path.
    //
    // std::atomic<bool> is used (not plain volatile) so the flag is correct
    // on dual-core targets (ESP32, ESP32-S3). On the C3 (UNICORE) the atomic
    // ops are single instructions and add no overhead vs volatile.
    std::atomic<bool> g_isrServiceInstalled{false};
}

void IsrPin::_ensureServiceInstalled() noexcept {
    // Acquire ordering on the load synchronizes-with the release store below.
    if (g_isrServiceInstalled.load(std::memory_order_acquire)) return;

    // Install with ESP_INTR_FLAG_IRAM so the shared dispatcher is resident
    // in IRAM. Required for any IRAM_ATTR handler to fire while flash cache
    // is disabled (e.g. during LittleFS write/erase or OTA flash).
    //
    // If another component already installed the service WITHOUT IRAM flag,
    // ESP-IDF returns ESP_ERR_INVALID_STATE and the dispatcher's IRAM-safety
    // is whatever that prior installer chose. We flip the flag regardless so
    // we don't retry forever; the deployment must avoid such double-install.
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);

    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        g_isrServiceInstalled.store(true, std::memory_order_release);
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
