#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// ApiClient: habla HTTPS con mTLS contra el gateway (backend Go en
// 34.120.207.250). Usa los certificados definidos en Certs.h/.cpp para
// el handshake mutuo (equivalente al --cert/--key que le pasan a Postman).
// ---------------------------------------------------------------------------
namespace ApiClient
{
    // POST /api/v1/devices
    // El "id" nunca se manda en el body: el backend lo saca del certificado
    // (Common Name) via mTLS, o del header X-Device-ID si el gateway tiene
    // mTLS apagado.
    // Devuelve true si el gateway respondio 2xx. Si se pasa responseBody,
    // ahi queda el JSON crudo de la respuesta.
    bool registerDevice(const String &serialNumber,
                         const String &lastSeen,
                         const String &lastBoot,
                         const String &city = "",
                         int detectionThreshold = -1,
                         const String &location = "",
                         const String &firmwareVersion = "",
                         String *responseBody = nullptr);

    // POST /api/v1/events
    // type debe ser "bell" o "motion". image es el string base64 de la foto.
    bool sendEvent(const String &type,
                   const String &imageBase64,
                   String *responseBody = nullptr);

    // Solo necesario si el gateway tiene mTLS APAGADO. En ese caso el
    // backend espera el device id en el header X-Device-ID en cada request.
    // Si el gateway usa mTLS real no usar esta función. Ya que se resuelve con los certs.
    void setDeviceIdHeader(const String &deviceId);
}
