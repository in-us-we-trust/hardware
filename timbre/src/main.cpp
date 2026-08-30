#include <Arduino.h>
#include <WiFi.h>
#include "Storage.h"
#include "Device.h"
#include "WifiManager.h"
#include "WebPortal.h"
#include "BleManager.h"
#include "Certs.h"
#include "CameraManager.h"
#include "DoorbellSession.h" // <-- unica capa de comunicacion que ve el main
#include "Screen.h"           // <-- pantalla de operacion (web + SSE)

// ---------------------------------------------------------------------------
// main.cpp: arranque del timbre + WiFi + (a futuro) deep sleep y boton. Toda
// la logica del protocolo (MQTT + RTSP + flujo del contrato) vive detras de
// DoorbellSession. El main no toca los transportes directamente.
//
// Para probar en Wokwi sin boton fisico: por el monitor serial,
//   'p' -> startCall() (timbrar)      'e' -> endCall() (terminar la llamada)
// ---------------------------------------------------------------------------

size_t getArduinoLoopTaskStackSize(void)
{
    return 16 * 1024;
}

static bool modoNormal = false;

// El gateway ordeno cortar por peligro. Aca iria la alerta local real.
void onDanger(const String &message)
{
    Serial.printf("[APP] ALERTA LOCAL: %s\n", message.c_str());
    // TODO fase 2: buzzer / luz / notificacion.
}

// Cada ia_response -> se muestra en la pantalla web.
void onMessage(const String &chunkType, const String &action,
               const String &severity, const String &message)
{
    String text = "[" + chunkType + "] " + action;
    if (message.length() > 0) text += " \u2014 " + message;
    Screen::showMessage(text, severity);
}

void setup()
{
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start < 4000))
    {
        delay(10);
    }
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
            Serial.println("[APP] serial: 'p' = timbrar (press), 'e' = terminar");

            Screen::begin(); // pantalla web (necesita WiFi ya conectado)

            DoorbellSession::begin(deviceId);
            DoorbellSession::onDanger(onDanger);
            DoorbellSession::onMessage(onMessage);

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

// Disparo de prueba por serial (reemplaza al boton fisico mientras no lo haya).
void handleSerialTrigger()
{
    if (!Serial.available())
    {
        return;
    }
    char c = Serial.read();
    if (c == 'p')
    {
        DoorbellSession::startCall();
    }
    else if (c == 'e')
    {
        DoorbellSession::endCall();
    }
}

void loop()
{
    if (modoNormal)
    {
        DoorbellSession::loop();
        Screen::handleClient();
        handleSerialTrigger();
        // A futuro: aca va el boton fisico -> DoorbellSession::startCall()
        // y el deep sleep cuando la sesion termina.
    }
    else
    {
        WebPortal::handleClient();
        BleManager::loop();
    }
}
