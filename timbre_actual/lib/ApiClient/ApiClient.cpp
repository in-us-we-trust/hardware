#include "ApiClient.h"
#include "Certs.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace
{
    // Host del gateway para SNI/verificacion TLS.
    const char *GATEWAY_HOST = "api.hogarseguro.local";
    const IPAddress GATEWAY_IP(34, 120, 207, 250);
    const uint16_t GATEWAY_PORT = 443;

    // Solo se usa si llaman a ApiClient::setDeviceIdHeader() (mTLS apagado).
    String deviceIdHeaderOverride = "";

    bool doPost(const String &path, const String &jsonBody, String *responseBody, bool acceptConflict)
{
    WiFiClientSecure client;
    
    // 1. TIMEOUT: Evita cortes a mitad del handshake mTLS
    client.setTimeout(15000); 
    client.setHandshakeTimeout(15000);
    client.setCACert(CA_CERT);
    client.setCertificate(DEVICE_CERT);
    client.setPrivateKey(DEVICE_KEY);

    String url = "https://" + String(GATEWAY_IP.toString().c_str()) + path;

    if (!client.connect(GATEWAY_IP, GATEWAY_PORT, GATEWAY_HOST, CA_CERT, DEVICE_CERT, DEVICE_KEY))
    {
        Serial.println("[ApiClient] No se pudo iniciar la conexion HTTPS");
        return false;
    }

    // k: [DEBUG] trazas temporales para ver qué request arma el cliente.
    Serial.println("[ApiClient] -------- Request debug --------");
    Serial.println("[ApiClient] path: " + path);
    Serial.println("[ApiClient] url: " + url);
    Serial.println("[ApiClient] header Content-Type: application/json");
    if (deviceIdHeaderOverride.length() > 0)
    {
        Serial.println("[ApiClient] header X-Device-ID: " + deviceIdHeaderOverride);
    }
    else
    {
        Serial.println("[ApiClient] header X-Device-ID: <no enviado>");
    }

    Serial.println("[ApiClient] POST " + url);
    Serial.println("[ApiClient] body:");
    Serial.println(jsonBody);
    Serial.println("[ApiClient] --------------------------------");

    String request;
    request.reserve(jsonBody.length() + 256);
    request += "POST ";
    request += path;
    request += " HTTP/1.1\r\nHost: ";
    request += GATEWAY_HOST;
    request += "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ";
    request += String(jsonBody.length());
    request += "\r\n";
    if (deviceIdHeaderOverride.length() > 0)
    {
        request += "X-Device-ID: ";
        request += deviceIdHeaderOverride;
        request += "\r\n";
    }
    request += "\r\n";
    request += jsonBody;

    size_t written = client.print(request);
    if (written == 0)
    {
        Serial.println("[ApiClient] Error enviando request HTTP");
        client.stop();
        return false;
    }

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();

    int statusCode = 0;
    if (statusLine.startsWith("HTTP/"))
    {
        int firstSpace = statusLine.indexOf(' ');
        int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
        if (firstSpace > 0)
        {
            String codeText = statusLine.substring(firstSpace + 1, secondSpace > firstSpace ? secondSpace : statusLine.length());
            statusCode = codeText.toInt();
        }
    }

    while (client.connected())
    {
        String headerLine = client.readStringUntil('\n');
        headerLine.trim();
        if (headerLine.length() == 0)
        {
            break;
        }
    }

    String body = client.readString();

    if (statusCode <= 0)
    {
        Serial.print("[ApiClient] Error de conexion/TLS: ");
        Serial.println("connection refused");
        client.stop();
        return false;
    }

    Serial.printf("[ApiClient] Status: %d\n", statusCode);
    Serial.println("[ApiClient] Respuesta: " + body);

    if (responseBody != nullptr)
    {
        *responseBody = body;
    }

    client.stop();

    if (acceptConflict && statusCode == 409)
    {
        Serial.println("[ApiClient] Device already exists; treating 409 as success for /devices");
        return true;
    }

    return statusCode >= 200 && statusCode < 300;
}
}

namespace ApiClient
{
    void setDeviceIdHeader(const String &deviceId)
    {
        deviceIdHeaderOverride = deviceId;
    }

    bool registerDevice(const String &serialNumber,
                         const String &lastSeen,
                         const String &lastBoot,
                         const String &city,
                         int detectionThreshold,
                         const String &location,
                         const String &firmwareVersion,
                         String *responseBody)
    {
        JsonDocument doc;
        doc["serialNumber"] = serialNumber;
        doc["lastSeen"] = lastSeen;
        doc["lastBoot"] = lastBoot;
        if (city.length() > 0)
            doc["city"] = city;
        if (detectionThreshold >= 0)
            doc["detectionThreshold"] = detectionThreshold;
        if (location.length() > 0)
            doc["location"] = location;
        if (firmwareVersion.length() > 0)
            doc["firmwareVersion"] = firmwareVersion;

        String jsonBody;
        serializeJson(doc, jsonBody);

        //  trazas temporales del body final que sale a /devices.
        Serial.println("[ApiClient] body final /devices:");
        Serial.println(jsonBody);

        return doPost("/api/v1/devices", jsonBody, responseBody, true);
    }

    bool sendEvent(const String &type,
                   const String &imageBase64,
                   String *responseBody)
    {
        // Armado manual (sin ArduinoJson) para no duplicar en RAM el base64
        // de la imagen.
        String jsonBody = "{\"type\":\"" + type + "\",\"image\":\"" + imageBase64 + "\"}";

        // trazas temporales del body final que sale a /events.
        Serial.println("[ApiClient] body final /events:");
        Serial.println(jsonBody);

        return doPost("/api/v1/events", jsonBody, responseBody, false);
    }
}
