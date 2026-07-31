#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// WifiManager: conexion del ESP32 a una red WiFi existente (modo STA).
// No confundir con el Access Point de configuracion, que maneja WebPortal.
// ---------------------------------------------------------------------------
namespace WifiManager
{
    // Intenta conectarse (20 intentos x 1s) y sincroniza la hora por NTP.
    // Devuelve true si conecto (independiente de si NTP sincronizo o no).
    bool connect(const String &ssid, const String &password);

    // Fecha/hora actual en ISO 8601 UTC, ej: "2026-06-27T18:30:00Z".
    // Requiere haber llamado connect() antes (necesita WiFi + NTP).
    String nowIso();
}
