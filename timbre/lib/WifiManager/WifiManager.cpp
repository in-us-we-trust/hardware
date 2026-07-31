#include "WifiManager.h"
#include <WiFi.h>
#include <time.h>

namespace
{
    // Sincroniza la hora por NTP. Necesaria para poder armar timestamps
    // ISO 8601 reales (lastSeen/lastBoot) para el gateway.
    void syncTime()
    {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");

        Serial.print("Sincronizando hora por NTP");
        time_t now = time(nullptr);
        int attempts = 0;
        // Antes de sincronizar, time() devuelve un valor muy chico (epoch~0).
        // Usamos una fecha "razonable" (año 2020) como umbral de exito.
        const time_t YEAR_2020 = 1577836800;
        while (now < YEAR_2020 && attempts < 40)
        {
            delay(500);
            Serial.print(".");
            now = time(nullptr);
            attempts++;
        }
        Serial.println();

        if (now < YEAR_2020)
        {
            Serial.println("No se pudo sincronizar la hora por NTP");

            // BORRAR: respaldo temporal para evitar que TLS quede en 1970 si
            // la sincronizacion NTP falla en este entorno.
            const time_t FALLBACK_UTC_2026_07_28 = 1785196800;
            time_t fallbackEpoch = FALLBACK_UTC_2026_07_28;
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
        while (WiFi.status() != WL_CONNECTED && attempts < 20)
        {
            delay(1000);
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
