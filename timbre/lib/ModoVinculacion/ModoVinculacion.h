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
    // Levanta portal web y BLE (en hardware real). Al terminar la vinculacion
    // BleManager reinicia y el boot entra en Modo::Operacion.
    void begin(const String &bleName);

    // Una vuelta: atiende el portal y el trabajo diferido de BLE.
    void loop();
}
