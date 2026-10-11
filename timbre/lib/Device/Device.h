#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Device: identidad del dispositivo y nombre BLE derivado del chip.
//
// El deviceId es FIJO (Config::Identity::ID, build flag DEVICE_ID) y tiene
// que ser el CN del certificado del dispositivo: Mosquitto lo usa como
// username y el ACL solo permite home/<CN>/#. Ya no se genera un UUID random.
// ---------------------------------------------------------------------------
namespace Device
{
    // Genera un UUID v4 nuevo (no lo persiste). Se usa para los trackId.
    String generateUUID();

    // deviceId del timbre (= CN del certificado).
    String getDeviceId();

    // true si el CN de DEVICE_CERT coincide con getDeviceId(). Si no coincide,
    // loguea el error: el broker aceptaria la conexion pero el ACL descartaria
    // todos los mensajes en silencio.
    bool certMatchesDeviceId();

    // Nombre BLE/AP derivado del MAC del chip, ej: "TIMBRE-A1B2C3"
    String getBleName();
}
