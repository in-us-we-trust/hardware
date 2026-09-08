#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Storage: unica responsable de leer/escribir en flash (Preferences/NVS).
// Nadie mas en el proyecto debe tocar Preferences directamente.
// ---------------------------------------------------------------------------
namespace Storage
{
    // --- Identidad del dispositivo (namespace "device") ---
    String loadDeviceId();
    void saveDeviceId(const String &id);
    bool loadDeviceRegistered();
    void saveDeviceRegistered(bool registered);

    // --- Credenciales WiFi (namespace "wifi") ---
    void saveWifiCredentials(const String &ssid, const String &password);
    String loadWifiSSID();
    String loadWifiPassword();
}
