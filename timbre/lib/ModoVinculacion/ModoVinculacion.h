#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// ModoVinculacion: el dispositivo todavia sin configurar (provisioning).
//
// Levanta el portal web (QR + SSE) y BLE para recibir las credenciales WiFi
// desde la app. Compone los singletons WebPortal y BleManager.
// ---------------------------------------------------------------------------
namespace ModoVinculacion
{
    // Levanta camara (en hardware real), portal web y BLE.
    void begin(const String &bleName);

    // Una vuelta: atiende el portal y el trabajo diferido de BLE.
    void loop();
}
