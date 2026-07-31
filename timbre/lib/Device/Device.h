#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Device: identidad del dispositivo (UUID estable y nombre BLE derivado del
// chip). Usa Storage para persistir el UUID entre reinicios.
// ---------------------------------------------------------------------------
namespace Device
{
    // Genera un UUID v4 nuevo (no lo persiste).
    String generateUUID();

    // Devuelve el UUID persistido; si no existe, lo genera y lo guarda.
    String getOrCreateDeviceId();

    // Nombre BLE/AP derivado del MAC del chip, ej: "TIMBRE-A1B2C3"
    String getBleName();
}
