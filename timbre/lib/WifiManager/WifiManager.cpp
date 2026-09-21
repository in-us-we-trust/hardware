#include "WifiManager.h"
#include "Config.h"
#include <WiFi.h>
#include <time.h>

namespace
{
    // Sincroniza la hora por NTP. Necesaria para poder armar timestamps
    // ISO 8601 reales (lastSeen/lastBoot) para el gateway.
    void syncTime()
    {
        configTime(0, 0, Config::Wifi::NTP_SERVER_1, Config::Wifi::NTP_SERVER_2);

        Serial.print("Sincronizando hora por NTP");
        time_t now = time(nullptr);
        int attempts = 0;
        // Antes de sincronizar, time() devuelve un valor muy chico (epoch~0).
        // Config::Wifi::NTP_MIN_VALID_EPOCH (año 2020) es el umbral de exito.
        while (now < Config::Wifi::NTP_MIN_VALID_EPOCH && attempts < Config::Wifi::NTP_ATTEMPTS)
        {
            delay(Config::Wifi::NTP_RETRY_MS);
            Serial.print(".");
            now = time(nullptr);
            attempts++;
        }
        Serial.println();

        if (now < Config::Wifi::NTP_MIN_VALID_EPOCH)
        {
            Serial.println("No se pudo sincronizar la hora por NTP");

            // Respaldo temporal para evitar que TLS quede en 1970 si la
            // sincronizacion NTP falla en este entorno. TODO: revisar si
            // sigue haciendo falta (ver Config::Wifi::NTP_FALLBACK_EPOCH).
            time_t fallbackEpoch = Config::Wifi::NTP_FALLBACK_EPOCH;
            struct timeval tv = { .tv_sec = fallbackEpoch, .tv_usec = 0 };
            settimeofday(&tv, nullptr);

            Serial.println("Hora de respaldo aplicada para permitir TLS");
        }
        else
        {
            Serial.println("Hora sincronizada");
        }
    }
}

namespace WifiManager
{
    bool connect(const String &ssid, const String &password)
    {
        Serial.println("\nConectando WiFi...");
        Serial.println(ssid);
        WiFi.begin(ssid.c_str(), password.c_str());
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < Config::Wifi::CONNECT_ATTEMPTS)
        {
            delay(Config::Wifi::CONNECT_RETRY_MS);
            Serial.print(".");
            attempts++;
        }
        Serial.println();
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("No se pudo conectar");
            return false;
        }

        Serial.println("WiFi conectado");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());

        syncTime();

        return true;
    }

    String nowIso()
    {
        time_t now = time(nullptr);
        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);
        char buf[25];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        return String(buf);
    }
}
