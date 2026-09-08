#include <Arduino.h>
#include <WiFi.h>
#include "Storage.h"
#include "Device.h"
#include "WifiManager.h"
#include "WebPortal.h"
#include "BleManager.h"
#include "Certs.h"
#include "CameraManager.h"
#include "LowPower.h"
#include "CallManager.h"
#include "AudioManager.h"

// Boton conectado a D0 (GPIO1) -> según el hardware del usuario
static const uint8_t BUTTON_PIN = 1;
volatile bool buttonPressed = false;
volatile uint32_t lastButtonMs = 0;

void IRAM_ATTR onButton()
{
    uint32_t now = millis();
    if (now - lastButtonMs < 200)
    {
        return; // Ignora TICs repetidos del pulsador
    }
    lastButtonMs = now;

    // Solo se dispara un START cuando NO hay llamada activa.
    // La finalizacion la hace la PC.
    if (!CallManager::inCall())
    {
        buttonPressed = true;
    }
}

size_t getArduinoLoopTaskStackSize(void)
{
    return 16 * 1024;
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

    String ssid = Storage::loadWifiSSID();
    String password = Storage::loadWifiPassword();

    String bleName = Device::getBleName();

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButton, FALLING);

    if (ssid.length() > 0)
    {
        Serial.println("Credenciales encontradas");
        if (WifiManager::connect(ssid, password))
        {
            Serial.println("\nModo normal (sin BLE ni AP)");

            // Nos mantenemos con BLE desactivado y entramos en modo de bajo consumo
            LowPower::enterIdleMode();
            AudioManager::begin();
            return;
        }

        Serial.println("No se pudo usar la red guardada; entrando en configuracion.");
    }

    Serial.println("\nEntrando en modo configuracion");
    WebPortal::begin(bleName);
    BleManager::begin(bleName);
}

void loop()
{
    WebPortal::handleClient();
    BleManager::loop(); // hace el trabajo pesado (WiFi + gateway) diferido desde BLE

    // Tambien debe ejecutarse cuando la PC acaba de colgar: el endpoint
    // marca la llamada como inactiva y handle() realiza la limpieza.
    CallManager::handle();
    AudioManager::loop();

    // Si se presiono el boton, iniciamos la llamada (si no hay una)
    if (buttonPressed)
    {
        buttonPressed = false;
        if (!CallManager::inCall())
        {
            CallManager::beginCall();
            Serial.println("[main] Boton: activado. La llamada sigue hasta que la PC la cuelgue.");
        }
        // Si ya hay llamada activa, ignoramos los TICs del botón.
    }

    if (CallManager::isHangupRequested())
    {
        Serial.println("[main] Hangup solicitado por la PC. Volviendo a reposo.");
        LowPower::enterIdleMode();
    }
}