#include <Arduino.h>
#include <WiFi.h>
#include "Storage.h"
#include "Device.h"
#include "WifiManager.h"
#include "WebPortal.h"
#include "BleManager.h"
#include "Certs.h"
#include "CameraManager.h"
#include "EventFlow.h"       // politica: que y cuando
#include "Screen.h"          // pantalla de operacion (web + SSE)
#include "MockMediaSource.h" // fuente de la foto (hardcodeada por ahora)

// ---------------------------------------------------------------------------
// main.cpp: arranque del timbre. En modo normal levanta las capas de comunica-
// cion. El flujo del evento vive en EventFlow; el transporte en DoorbellSession.
//
// Prueba en Wokwi sin boton fisico:
//   'p' -> onTimbre() (toca el timbre)   'e' -> terminar() (cierra la sesion)
// ---------------------------------------------------------------------------

size_t getArduinoLoopTaskStackSize(void) { return 16 * 1024; }

static bool modoNormal = false;

// Fuente de la foto. Hoy mock (JPEG embebido); manana CameraMediaSource.
static MockMediaSource fotoSource(1);

void setup()
{
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start < 4000))
        delay(10);
    Serial.println("\nIniciando...");

#ifdef WOKWI_SEED_NVS
    Storage::saveWifiCredentials(WOKWI_SEED_SSID, WOKWI_SEED_PASS);
    Storage::saveDeviceRegistered(WOKWI_SEED_REGISTERED);
    Serial.println("[WOKWI] NVS sembrado con SSID: " WOKWI_SEED_SSID);
#endif

    String ssid = Storage::loadWifiSSID();
    String password = Storage::loadWifiPassword();
    String bleName = Device::getBleName();

    if (ssid.length() > 0)
    {
        Serial.println("Credenciales encontradas");
        if (WifiManager::connect(ssid, password))
        {
            Serial.println("\nModo normal (sin BLE ni AP)");

            String deviceId = Device::getOrCreateDeviceId();
            Serial.printf("[APP] deviceId = %s\n", deviceId.c_str());
            Serial.println("[APP] serial: 'p' = timbrar, 'e' = terminar");

            Screen::begin();                         // pantalla web (necesita WiFi)
            EventFlow::begin(deviceId, &fotoSource); // politica + transporte + fuente

            modoNormal = true;
            return;
        }
    }

    Serial.println("\nEntrando en modo configuracion");
#ifndef WOKWI_SIM
    CameraManager::begin();
#endif
    WebPortal::begin(bleName);
#ifndef WOKWI_SIM
    BleManager::begin(bleName);
#endif
}

// Disparo de prueba por serial (reemplaza al boton fisico).
void handleSerialTrigger()
{
    if (!Serial.available())
        return;
    char c = Serial.read();
    if (c == 'p')
        EventFlow::onTimbre();
    else if (c == 'e')
        EventFlow::terminar();
}

void loop()
{
    if (modoNormal)
    {
        EventFlow::loop();
        Screen::handleClient(); // atiende la pagina web
        handleSerialTrigger();
    }
    else
    {
        WebPortal::handleClient();
        BleManager::loop();
    }
}
