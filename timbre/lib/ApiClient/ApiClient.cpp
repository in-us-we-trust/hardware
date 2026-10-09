#include "ApiClient.h"
#include "Certs.h"
#include "Config.h"
#include "Device.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace
{
    // Gateway: Config::Api::URL partida en esquema/host/puerto/path base.
    struct Endpoint
    {
        bool https = true;
        String host;
        uint16_t port = 443;
        String basePath; // sin '/' final ("" o "/algo")
    };

    Endpoint parseUrl(String url)
    {
        Endpoint e;
        if (url.startsWith("http://"))
        {
            e.https = false;
            e.port = 80;
            url = url.substring(7);
        }
        else if (url.startsWith("https://"))
            url = url.substring(8);

        int slash = url.indexOf('/');
        String hostPort = slash >= 0 ? url.substring(0, slash) : url;
        e.basePath = slash >= 0 ? url.substring(slash) : "";
        while (e.basePath.endsWith("/"))
            e.basePath.remove(e.basePath.length() - 1);

        int colon = hostPort.indexOf(':');
        if (colon >= 0)
        {
            e.host = hostPort.substring(0, colon);
            e.port = hostPort.substring(colon + 1).toInt();
        }
        else
            e.host = hostPort;
        return e;
    }

    const Endpoint &gateway()
    {
        static Endpoint e = parseUrl(Config::Api::URL);
        return e;
    }

    // Solo se usa si llaman a ApiClient::setDeviceIdHeader() (mTLS apagado).
    String deviceIdHeaderOverride = "";

    bool doPost(const String &path, const String &jsonBody, String *responseBody, bool acceptConflict)
{
    const Endpoint &gw = gateway();
    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    WiFiClient &client = gw.https ? static_cast<WiFiClient &>(secureClient) : plainClient;
    String hostHeader = gw.host;
    String fullPath = gw.basePath + path;

    Serial.printf("[ApiClient] POST %s://%s:%u%s\n", gw.https ? "https" : "http",
                  gw.host.c_str(), gw.port, fullPath.c_str());

    if (gw.https)
    {
        // TIMEOUT: Evita cortes a mitad del handshake mTLS
        secureClient.setTimeout(Config::Api::TLS_TIMEOUT_MS);
        secureClient.setHandshakeTimeout(Config::Api::TLS_TIMEOUT_MS);
        secureClient.setCACert(CA_CERT);
        secureClient.setCertificate(DEVICE_CERT);
        secureClient.setPrivateKey(DEVICE_KEY);

        // SNI / validacion del cert: TLS_HOSTNAME si esta, si no el host de la URL.
        if (strlen(Config::Api::TLS_HOSTNAME) > 0)
            hostHeader = Config::Api::TLS_HOSTNAME;

        IPAddress ip;
        if (!WiFi.hostByName(gw.host.c_str(), ip))
        {
            Serial.println("[ApiClient] No se pudo resolver " + gw.host);
            return false;
        }
        if (!secureClient.connect(ip, gw.port, hostHeader.c_str(), CA_CERT, DEVICE_CERT, DEVICE_KEY))
        {
            Serial.println("[ApiClient] No se pudo iniciar la conexion HTTPS");
            return false;
        }
    }
    else
    {
        plainClient.setTimeout(Config::Api::TLS_TIMEOUT_MS / 1000); // en segundos
        if (!plainClient.connect(gw.host.c_str(), gw.port))
        {
            Serial.println("[ApiClient] No se pudo iniciar la conexion HTTP");
            return false;
        }
    }

    // Sin TLS el gateway no tiene el cert para sacar el id: va en X-Device-ID.
    String deviceIdHeader = deviceIdHeaderOverride;
    if (deviceIdHeader.length() == 0 && !gw.https)
        deviceIdHeader = Device::getDeviceId();

    // k: [DEBUG] trazas temporales para ver qué request arma el cliente.
    Serial.println("[ApiClient] -------- Request debug --------");
    Serial.println("[ApiClient] path: " + fullPath);
    Serial.println("[ApiClient] header Host: " + hostHeader);
    Serial.println("[ApiClient] header Content-Type: application/json");
    if (deviceIdHeader.length() > 0)
    {
        Serial.println("[ApiClient] header X-Device-ID: " + deviceIdHeader);
    }
    else
    {
        Serial.println("[ApiClient] header X-Device-ID: <no enviado>");
    }

    Serial.println("[ApiClient] body:");
    Serial.println(jsonBody);
    Serial.println("[ApiClient] --------------------------------");

    String request;
    request.reserve(jsonBody.length() + 256);
    request += "POST ";
    request += fullPath;
    request += " HTTP/1.1\r\nHost: ";
    request += hostHeader;
    request += "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ";
    request += String(jsonBody.length());
    request += "\r\n";
    if (deviceIdHeader.length() > 0)
    {
        request += "X-Device-ID: ";
        request += deviceIdHeader;
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
        Serial.printf("[ApiClient] Respuesta invalida/sin respuesta del gateway (statusLine='%s')\n",
                      statusLine.c_str());
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

        return doPost(Config::Api::PATH_DEVICES, jsonBody, responseBody, true);
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

        return doPost(Config::Api::PATH_EVENTS, jsonBody, responseBody, false);
    }
}
