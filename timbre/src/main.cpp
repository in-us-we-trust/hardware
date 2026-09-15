#include <Arduino.h>
#include <WiFi.h>
#include "Storage.h"
#include "Device.h"
#include "WifiManager.h"
#include "WebPortal.h"
#include "BleManager.h"
#include "Certs.h"
#include "CameraSource.h"
#include "EventFlow.h"       // politica + transporte + stream local (coordinador unico)
#include "Screen.h"          // pantalla de operacion (web + SSE)
#include "LowPower.h"        // reposo (bajo consumo)
#include "MockMediaSource.h" // fuente de la foto (hardcodeada por ahora)

// ---------------------------------------------------------------------------
// main.cpp: arranque del timbre. En modo normal levanta las capas de comunica-
// cion y arranca en reposo. El evento (backend + stream local en paralelo) vive
// en EventFlow; el transporte de backend en DoorbellSession.
//
// Disparo del evento:
//   - Boton fisico en D0 (GPIO1), flanco de bajada, debounce 200ms.
//   - Serial (para Wokwi, sin boton): 'p' = timbrar, 'e' = colgar (local).
// El colgado real lo hace la PC (endpoint /hangup del stream local).
// ---------------------------------------------------------------------------

// >>> PROVISIONING TEMPORAL A MANO <<<
// Siembra las credenciales en NVS en cada boot. Sirve para provisionar sin BLE
// ni portal. La red debe ser 2.4GHz. Cuando el timbre ya conecte, poné
// SEED_WIFI_MANUAL en 0 y reflashea; las credenciales quedan guardadas en NVS.
#define SEED_WIFI_MANUAL 1
#define SEED_WIFI_SSID "mi-wifi"
#define SEED_WIFI_PASS "mi-psw"

size_t getArduinoLoopTaskStackSize(void) { return 16 * 1024; }

static bool modoNormal = false;

// Boton conectado a D0 (GPIO1).
static const uint8_t BUTTON_PIN = 1;
volatile bool buttonPressed = false;
volatile uint32_t lastButtonMs = 0;

// Fuente de la foto. Hoy mock (JPEG embebido); manana CameraMediaSource.
static MockMediaSource fotoSource(1);

void IRAM_ATTR onButton()
{
    uint32_t now = millis();
    if (now - lastButtonMs < 200)
        return; // ignora rebotes del pulsador
    lastButtonMs = now;
    buttonPressed = true; // EventFlow::onTimbre ignora el disparo si ya hay evento
}

void setup()
{
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start < 4000))
        delay(10);
    Serial.println("\nIniciando...");

#if SEED_WIFI_MANUAL
    Storage::saveWifiCredentials(SEED_WIFI_SSID, SEED_WIFI_PASS);
    Storage::saveDeviceRegistered(true);
    Serial.println("[APP] credenciales sembradas en NVS: " SEED_WIFI_SSID);
#endif

    String ssid = Storage::loadWifiSSID();
    String password = Storage::loadWifiPassword();
    String bleName = Device::getBleName();

    // Boton fisico (no existe en Wokwi, pero attachInterrupt es inocuo alli).
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButton, FALLING);

    if (ssid.length() > 0)
    {
        Serial.println("Credenciales encontradas");
        if (WifiManager::connect(ssid, password))
        {
            Serial.println("\nModo normal (sin BLE ni AP)");

            String deviceId = Device::getOrCreateDeviceId();
            Serial.printf("[APP] deviceId = %s\n", deviceId.c_str());
            Serial.println("[APP] serial: 'p' = timbrar, 'e' = colgar (local)");

            Screen::begin();                         // pantalla web (necesita WiFi)
            EventFlow::begin(deviceId, &fotoSource); // coordinador unico

            // Arrancamos en reposo: camara off, CPU 80MHz, WiFi modem sleep.
            LowPower::enterIdleMode();

            modoNormal = true;
            return;
        }
    }

    Serial.println("\nEntrando en modo configuracion");
#ifndef WOKWI_SIM
    Camera.begin();
#endif
    WebPortal::begin(bleName);
#ifndef WOKWI_SIM
    BleManager::begin(bleName);
#endif
}

// Disparo de prueba por serial (reemplaza al boton en Wokwi).
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
        EventFlow::loop();      // backend + stream local
        Screen::handleClient(); // atiende la pagina de estado (SSE)
        handleSerialTrigger();

        if (buttonPressed)
        {
            buttonPressed = false;
            EventFlow::onTimbre(); // ignora el disparo si ya hay un evento
        }
    }
    else
    {
        WebPortal::handleClient();
        BleManager::loop();
    }
}
