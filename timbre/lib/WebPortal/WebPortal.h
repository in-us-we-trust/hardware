#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// WebPortal: Access Point + servidor web de configuracion. Muestra el QR
// con el nombre BLE y transmite el estado del emparejamiento por SSE.
// ---------------------------------------------------------------------------
namespace WebPortal
{
    // Levanta el AP y el servidor web (rutas /, /qr.js, /events).
    void begin(const String &bleName);

    // Apaga el AP/servidor para liberar memoria cuando ya no hace falta.
    void stop();

    // Debe llamarse en cada loop() para atender clientes HTTP.
    void handleClient();

    // Envia un mensaje de estado al cliente conectado por SSE (y a Serial).
    void sendSSE(const String &message);
}
