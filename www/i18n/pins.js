/**
 * /www/i18n/pins.js  –  the shared pin field and board header (EN/BG)
 *
 * Used by /js/pins.js on every page with a pin: first-run, Hardware, the
 * sensor editor and the add-sensor wizard. Loaded by index.html and
 * firstrun.html right after common.js.
 */
"use strict";

I18n.register("pins", {
  en: {
    range: "not on this chip (GPIO 0–{max})",
    flash: "SPI flash bus, never usable",
    strap: "boot strap: must not be held low at reset",
    uart: "UART0, the serial console",
    usb: "USB D-/D+",
    absent: "no header pad on this board",
    unknown: "“{v}” is not a pin on this board",
    required: "required",
    unset: "not used",
    mean: " Did you mean D{g} (GPIO{to})?",
    dup: "also used by {w}",
    lgUse: "used",
    lgWarn: "use with care",
    lgBad: "never usable",
    ph: "label or GPIO",
    boardTitle: "Board pins",
    boardHint: "Type a pin the way the board prints it (D6) or as a GPIO (12). Yellow pins work with the right wiring; red ones never do.",
    gridHint: "This profile has no single board to draw, so every GPIO of the chip is shown.",
    fixPins: "Fix the pins marked in red.",
    unsafeAuto: "A yellow pin is in use: this sensor is saved with allow_unsafe_pins, so the firmware accepts it.",
    sdCard: "SD card",
    wifiButton: "WiFi button",
    ffButton: "FF button",
    pfButton: "PF button",
    flow: "Flow sensor",
    rtc: "RTC",
  },
  bg: {
    range: "няма го на този чип (GPIO 0–{max})",
    flash: "SPI флаш шина, никога не става",
    strap: "boot strap: не бива да е LOW при reset",
    uart: "UART0, серийната конзола",
    usb: "USB D-/D+",
    absent: "няма пад на тази платка",
    unknown: "„{v}“ не е пин на тази платка",
    required: "задължително",
    unset: "не се ползва",
    mean: " Може би D{g} (GPIO{to})?",
    dup: "ползва се и от {w}",
    lgUse: "зает",
    lgWarn: "внимателно",
    lgBad: "не става",
    ph: "етикет или GPIO",
    boardTitle: "Пинове на платката",
    boardHint: "Пиши пина както е на платката (D6) или като GPIO (12). Жълтите стават с правилно свързване, червените никога.",
    gridHint: "Този профил няма конкретна платка за рисуване, затова са показани всички GPIO на чипа.",
    fixPins: "Поправи пиновете, отбелязани в червено.",
    unsafeAuto: "Ползва се жълт пин: сензорът се записва с allow_unsafe_pins, за да го приеме firmware-ът.",
    sdCard: "SD карта",
    wifiButton: "WiFi бутон",
    ffButton: "FF бутон",
    pfButton: "PF бутон",
    flow: "Разходомер",
    rtc: "RTC",
  },
});
