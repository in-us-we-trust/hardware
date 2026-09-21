#pragma once
#include <Arduino.h>
#include <time.h> // time_t (umbrales NTP)

// =============================================================================
// Config.h — Punto UNICO para las "opciones raras" del timbre.
//
// Antes estos valores estaban desperdigados como #define y numeros magicos en
// main.cpp, WifiManager, EventFlow, BackendSession, RtspPublisher, ApiClient...
// Aca quedan juntos, con nombre y agrupados por tema.
//
// Regla de oro para saber que va donde:
//   - Valor FIJO del firmware (pines, timeouts, tamaños de buffer, reintentos):
//     vive aca como constexpr.
//   - Endpoint que CAMBIA por entorno (dev/prod/wokwi): se define por build flag
//     en platformio.ini. Aca solo damos UN default de respaldo y lo exponemos
//     como constexpr, para que el codigo lea siempre desde el mismo lugar y no
//     haya dos defaults distintos dando vueltas (bug historico: MEDIAMTX_HOST
//     valia "192.168.1.50" en BackendSession y "192.168.1.69" en RtspPublisher).
// =============================================================================

// --- Defaults de red: override real por build flags en platformio.ini --------
// Si el flag no esta definido, valen estos. Definidos UNA sola vez, aca.
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "test.mosquitto.org"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif
#ifndef MEDIAMTX_HOST
#define MEDIAMTX_HOST "192.168.1.69"
#endif
#ifndef MEDIAMTX_PORT
#define MEDIAMTX_PORT 8554
#endif
#ifndef MEDIAMTX_PATH
#define MEDIAMTX_PATH "doorbell"
#endif

namespace Config
{
    // --- Entorno de build: hardware real vs. simulador Wokwi ----------------
    // Wokwi no simula camara, BLE ni modo AP. En vez de #ifdef WOKWI_SIM
    // repartidos por el codigo, un solo bool que se puede usar con "if" normal.
    namespace Env
    {
#ifdef WOKWI_SIM
        constexpr bool IS_WOKWI_SIM = true;
#else
        constexpr bool IS_WOKWI_SIM = false;
#endif
    }

    // --- Provisioning temporal a mano (seed de WiFi por build flags) --------
    // SEED_WIFI_SSID / SEED_WIFI_PASS / SEED_WIFI_REGISTERED se definen (o no)
    // en platformio.ini, nunca en el fuente. El #if defined(...) se resuelve
    // UNA sola vez, aca. El resto del codigo (main.cpp) solo lee
    // Config::Build::SEED_WIFI_ENABLED con un "if" comun — no vuelve a ver
    // un #ifdef.
    namespace Build
    {
#if defined(SEED_WIFI_SSID) && defined(SEED_WIFI_PASS)
        constexpr bool SEED_WIFI_ENABLED = true;
        constexpr const char *SEED_SSID = SEED_WIFI_SSID;
        constexpr const char *SEED_PASSWORD = SEED_WIFI_PASS;
#else
        constexpr bool SEED_WIFI_ENABLED = false;
        constexpr const char *SEED_SSID = "";
        constexpr const char *SEED_PASSWORD = "";
#endif

#ifdef SEED_WIFI_REGISTERED
        constexpr bool SEED_DEVICE_REGISTERED = (SEED_WIFI_REGISTERED != 0);
#else
        constexpr bool SEED_DEVICE_REGISTERED = false;
#endif
    }

    // --- Boton fisico (timbre) ----------------------------------------------
    namespace Button
    {
        constexpr uint8_t PIN = 1;            // D0 / GPIO1
        constexpr uint32_t DEBOUNCE_MS = 200; // rebotes del pulsador a ignorar
    }

    // --- Arranque / sistema -------------------------------------------------
    namespace System
    {
        constexpr uint32_t SERIAL_BAUD = 115200;
        constexpr uint32_t SERIAL_WAIT_MS = 4000; // espera al USB CDC en boot
        constexpr size_t LOOP_TASK_STACK = 16 * 1024;
        constexpr uint32_t CPU_MHZ_ACTIVE = 240; // durante un evento
        constexpr uint32_t CPU_MHZ_IDLE = 80;    // en reposo (bajo consumo)
    }

    // --- WiFi + NTP (WifiManager) -------------------------------------------
    namespace Wifi
    {
        constexpr int CONNECT_ATTEMPTS = 20;
        constexpr uint32_t CONNECT_RETRY_MS = 1000;

        constexpr int NTP_ATTEMPTS = 40;
        constexpr uint32_t NTP_RETRY_MS = 500;
        // Umbral para dar la hora por sincronizada (epoch de 2020-01-01).
        constexpr time_t NTP_MIN_VALID_EPOCH = 1577836800;
        // Respaldo temporal si NTP falla, para que TLS no quede en 1970.
        // (era el "// BORRAR" de WifiManager: revisar si sigue haciendo falta)
        constexpr time_t NTP_FALLBACK_EPOCH = 1785196800; // 2026-07-28 UTC
        constexpr const char *NTP_SERVER_1 = "pool.ntp.org";
        constexpr const char *NTP_SERVER_2 = "time.nist.gov";
    }

    // --- Evento: coordinacion y stream local (EventFlow) --------------------
    namespace Event
    {
        constexpr uint32_t BACKEND_CONNECT_TIMEOUT_MS = 10000;
        constexpr uint32_t AUDIO_LEVEL_THROTTLE_MS = 120; // push nivel a Screen

        // Con el stream local ya corriendo, cada cuanto se re-evalua si hay
        // motivo (sonido o persona) para CONECTAR al backend. El primer chequeo
        // cae un intervalo despues del timbre, lo que da warm-up a camara/mic.
        constexpr uint32_t EVAL_INTERVAL_MS = 1000;

        // Cada cuanto EventFlow re-evalua que mandar por RTSP. Audio y video
        // van a distinta frecuencia a proposito:
        //  - Audio: leer el nivel del mic es baratisimo y un sonido corto no
        //    debe pasar entre chequeos, asi que se evalua seguido.
        //  - Video: el tiny model es caro y comparte la camara con MJPEG/RTSP,
        //    asi que se evalua espaciado.
        constexpr uint32_t AUDIO_DETECT_INTERVAL_MS = 50;
        constexpr uint32_t VIDEO_DETECT_INTERVAL_MS = 500;
        // Umbral de nivel de microfono (0..32767) para considerar que "hay
        // sonido" y empezar a mandar audio. TODO: ajustar en campo (el gate de
        // ruido base de la pantalla ronda 800).
        constexpr uint16_t AUDIO_ACTIVATE_LEVEL = 800;
    }

    // --- Stream MJPEG local (EventFlow, core 0) -----------------------------
    namespace Mjpeg
    {
        constexpr uint16_t PORT = 8081;
        constexpr size_t BUF_SIZE = 100 * 1024;
        constexpr uint32_t TASK_STACK = 8192;
        constexpr BaseType_t TASK_CORE = 0;
        constexpr uint32_t FRAME_DELAY_MS = 40; // ~25 fps tope
        constexpr uint32_t ACCEPT_IDLE_MS = 50; // sin cliente conectado
        constexpr int STOP_WAIT_TICKS = 40;     // x 50ms al frenar la task
        constexpr uint32_t STOP_WAIT_MS = 50;
    }

    // --- Pagina HTTP del dispositivo (:80) ----------------------------------
    // La misma la sirven WebPortal (modo config/vinculacion) y Screen (modo
    // operacion). Nunca conviven: son el mismo puerto en modos distintos.
    namespace Http
    {
        constexpr uint16_t PORT = 80;
    }

    // --- MQTT (BackendSession -> MqttClient) --------------------------------
    namespace Mqtt
    {
        constexpr const char *BROKER_HOST = MQTT_BROKER_HOST;
        constexpr uint16_t BROKER_PORT = MQTT_BROKER_PORT;
    }

    // --- MediaMTX / RTSP (BackendSession, RtspPublisher) -------------------
    namespace Media
    {
        constexpr const char *HOST = MEDIAMTX_HOST;
        constexpr uint16_t PORT = MEDIAMTX_PORT;
        constexpr const char *PATH = MEDIAMTX_PATH;
    }

    // --- Gateway HTTP mTLS (ApiClient) --------------------------------------
    namespace Api
    {
        constexpr const char *GATEWAY_HOST = "api.hogarseguro.local"; // SNI/TLS
        // IP publica del gateway (el host .local no resuelve por DNS).
        constexpr uint8_t GATEWAY_IP[4] = {34, 120, 207, 250};
        constexpr uint16_t GATEWAY_PORT = 443;
        constexpr uint32_t TLS_TIMEOUT_MS = 15000;
        constexpr const char *PATH_DEVICES = "/api/v1/devices";
        constexpr const char *PATH_EVENTS = "/api/v1/events";
    }
}
