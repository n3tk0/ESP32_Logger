#pragma once
// Host shim for <WiFi.h>: the getters a module's status chip reads, answering
// as a radio that is not connected. Enough for WiFiModule.cpp to compile; its
// load()/save() are what the host tests exercise, and they touch no radio.
#include <Arduino.h>

enum { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 };

class IPAddress {
public:
    String toString() const { return String("0.0.0.0"); }
};

class HostWiFi {
public:
    int       status()   const { return WL_DISCONNECTED; }
    String    SSID()     const { return String(); }
    IPAddress localIP()  const { return IPAddress(); }
    IPAddress softAPIP() const { return IPAddress(); }
    long      RSSI()     const { return 0; }
};
inline HostWiFi WiFi;
