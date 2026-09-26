#include "ApiClient.h"
#include "Certs.h"
#include "Config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace
{
    // Gateway (host/IP/puerto): definidos en Config::Api. La IP se arma aca
    // como IPAddress a partir de los 4 octetos (IPAddress no es constexpr).
    const char *GATEWAY_HOST = Config::Api::GATEWAY_HOST;
    const IPAddress GATEWAY_IP(Config::Api::GATEWAY_IP[0], Config::Api::GATEWAY_IP[1],
                              Config::Api::GATEWAY_IP[2], Config::Api::GATEWAY_IP[3]);
    const uint16_t GATEWAY_PORT = Config::Api::GATEWAY_PORT;

    // Solo se usa si llaman a ApiClient::setDeviceIdHeader() (mTLS apagado).
    String deviceIdHeaderOverride = "";

    bool doPost(const String &path, const String &jsonBody, String *responseBody, bool acceptConflict)
{
    WiFiClientSecure client;

    // TIMEOUT: Evita cortes a mitad del handshake mTLS
    client.setTimeout(Config::Api::TLS_TIMEOUT_MS);
    client.setHandshakeTimeout(Config::Api::TLS_TIMEOUT_MS);
    client.setCACert(CA_CERT);
    client.setCertificate(DEVICE_CERT);
    client.setPrivateKey(DEVICE_KEY);

    String url = "https://" + String(GATEWAY_IP.toString().c_str()) + path;

    if (!client.connect(GATEWAY_IP, GATEWAY_PORT, GATEWAY_HOST, CA_CERT, DEVICE_CERT, DEVICE_KEY))
    {
        Serial.println("[ApiClient] No se pudo iniciar la conexion HTTPS");
        return false;
    }

    if (deviceIdHeaderOverride.length() > 0)
    {
        Serial.println("[ApiClient] header X-Device-ID: " + deviceIdHeaderOverride);
    }
    else
    {
        Serial.println("[ApiClient] header X-Device-ID: <no enviado>");
    }

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

        return doPost(Config::Api::PATH_DEVICES, jsonBody, responseBody, true);
    }

    bool sendEvent(const String &type,
                   const String &imageBase64,
                   String *responseBody)
    {
        String jsonBody = "{\"type\":\"" + type + "\",\"image\":\"" + imageBase64 + "\"}";

        return doPost(Config::Api::PATH_EVENTS, jsonBody, responseBody, false);
    }
}
