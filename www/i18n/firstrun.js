/**
 * /www/i18n/firstrun.js — strings for /firstrun.html (captive-portal wizard)
 *
 * firstrun.html is a standalone document (its own <head>, not part of the
 * index.html SPA shell) served unauthenticated before the device is
 * configured. It loads i18n.js + common.js + this file directly, in that
 * order, ahead of js/firstrun.js.
 */
"use strict";

I18n.register("firstrun", {
  en: {
    title: "First-run Setup",
    subtitle: "Welcome. Pick your board and assign pins before the logger starts.",

    step1Title: "1. Select your board",
    profileLabel: "Board profile",
    profileLoading: "— Loading profiles… —",
    chooseBoard: "— Choose a board —",
    customDisclaimer: "I select <strong>Custom</strong> and accept that pin validation is disabled. By continuing you accept full responsibility for any boot-mode or hardware damage caused by unsafe pin assignments.",
    customValidationOff: "Validation disabled. Any GPIO 0–48 allowed.",
    hintSummary: "Strap: {strap}  •  USB: {usb}  •  max GPIO: {max}",
    hintNoHeaderPad: "  •  no header pad: {pins}",
    none: "none",

    step2Title: "2. Operating mode",
    modeLabel: "Mode",
    modeLegacyOption: "Legacy — single flow meter with deep sleep",
    modeContinuousOption: "Continuous — always-on sensor pipeline",
    modeHybridOption: "Hybrid — both flow + sensor pipeline",
    modeHint: "Legacy and Hybrid require GPIO pin assignments for the flow pipeline. Continuous configures sensors individually in the main UI later.",

    step3Title: "3. Pin assignments",
    pinsHint: "The WiFi-trigger and wake-up buttons drive the device's physical UI in every mode. Flow sensor and DS1302 RTC pins are only collected for legacy/hybrid modes (hidden when “Continuous” is selected). Leave any optional field at -1 to skip. Each pin is validated against the selected board's restriction list.",

    pinWifiTrigger: "WiFi-trigger button",
    pinWakeupFF: "Wakeup (FF / manual)",
    pinWakeupPF: "Wakeup (PF / auto)",
    pinFlowSensor: "Flow sensor input",
    pinRtcCE: "RTC chip-enable (DS1302)",
    pinRtcIO: "RTC data IO",
    pinRtcSCLK: "RTC clock",

    reasonNoProfile: "no board profile selected",
    reasonUnassigned: "unassigned (optional)",
    reasonNegative: "negative GPIO",
    reasonMaxGpio: "GPIO > {max} for this board",
    reasonCustomOff: "custom — validation off",
    reasonBootstrap: "bootstrap pin (boot-mode risk)",
    reasonUsbCdc: "USB CDC pin (D+/D-)",
    reasonSpiFlash: "SPI flash bus pin",
    reasonUart0: "UART0 console (you would lose serial debug)",
    reasonAbsent: "not broken out on this board",
    reasonOk: "ok",
    reasonDuplicate: "duplicate of {label}",
    reasonRequired: "required",

    saveBtn: "Save & reboot",
    saveBtnAria: "Save configuration and reboot the device",

    statusLoadProfilesFailed: "Failed to load board profiles: {msg}",
    statusPickProfile: "Pick a board profile first.",
    statusCheckCustomAck: "Check the Custom acknowledgement to proceed.",
    statusFixPins: "Fix the highlighted pins above.",
    statusSaving: "Saving and rebooting…",
    statusError: "Error: {error}",
    statusSaved: "Saved. Device is rebooting — this page will reload in 8 seconds.",
    statusNetworkError: "Network error: {msg}",
  },
  bg: {
    title: "Първоначално настройване",
    subtitle: "Добре дошли. Изберете платка и разпределете пиновете, преди логърът да стартира.",

    step1Title: "1. Изберете платка",
    profileLabel: "Профил на платката",
    profileLoading: "— Зареждане на профили… —",
    chooseBoard: "— Изберете платка —",
    customDisclaimer: "Избирам <strong>Custom</strong> и приемам, че валидацията на пиновете е изключена. С продължаването поемам пълна отговорност за всякакви повреди от boot-mode конфликт или хардуерна щета, причинени от небезопасно разпределение на пиновете.",
    customValidationOff: "Валидацията е изключена. Разрешени са всички GPIO 0–48.",
    hintSummary: "Strap: {strap}  •  USB: {usb}  •  макс. GPIO: {max}",
    hintNoHeaderPad: "  •  без извод на платката: {pins}",
    none: "няма",

    step2Title: "2. Режим на работа",
    modeLabel: "Режим",
    modeLegacyOption: "Legacy — единичен разходомер с дълбок сън",
    modeContinuousOption: "Continuous — постоянно активен пайплайн от сензори",
    modeHybridOption: "Hybrid — разходомер + пайплайн от сензори",
    modeHint: "Legacy и Hybrid изискват разпределение на GPIO пинове за разходомерния пайплайн. Continuous настройва сензорите поотделно по-късно в основния интерфейс.",

    step3Title: "3. Разпределение на пиновете",
    pinsHint: "Бутоните за WiFi-тригер и събуждане управляват физическия интерфейс на устройството във всеки режим. Пиновете за разходомер и DS1302 RTC се събират само за режими legacy/hybrid (скрити при избран „Continuous“). Оставете всяко незадължително поле на -1, за да го пропуснете. Всеки пин се валидира спрямо ограничителния списък на избраната платка.",

    pinWifiTrigger: "Бутон WiFi-тригер",
    pinWakeupFF: "Събуждане (FF / ръчно)",
    pinWakeupPF: "Събуждане (PF / автоматично)",
    pinFlowSensor: "Вход на разходомера",
    pinRtcCE: "RTC chip-enable (DS1302)",
    pinRtcIO: "RTC данни (IO)",
    pinRtcSCLK: "RTC clock",

    reasonNoProfile: "не е избран профил на платка",
    reasonUnassigned: "незададен (незадължителен)",
    reasonNegative: "отрицателен GPIO",
    reasonMaxGpio: "GPIO > {max} за тази платка",
    reasonCustomOff: "custom — валидацията е изключена",
    reasonBootstrap: "bootstrap пин (риск за boot-mode)",
    reasonUsbCdc: "USB CDC пин (D+/D-)",
    reasonSpiFlash: "пин от SPI flash шината",
    reasonUart0: "UART0 конзола (ще загубите сериен дебъг)",
    reasonAbsent: "не е изведен на тази платка",
    reasonOk: "ok",
    reasonDuplicate: "дублира {label}",
    reasonRequired: "задължително",

    saveBtn: "Запази и рестартирай",
    saveBtnAria: "Запази конфигурацията и рестартирай устройството",

    statusLoadProfilesFailed: "Неуспешно зареждане на профилите на платки: {msg}",
    statusPickProfile: "Първо изберете профил на платка.",
    statusCheckCustomAck: "Отметнете потвърждението за Custom, за да продължите.",
    statusFixPins: "Оправете маркираните по-горе пинове.",
    statusSaving: "Записва се и се рестартира…",
    statusError: "Грешка: {error}",
    statusSaved: "Запазено. Устройството се рестартира — страницата ще се презареди след 8 секунди.",
    statusNetworkError: "Мрежова грешка: {msg}",
  },
});
