#include <Arduino.h>
#include "Storage.h"
#include "Device.h"
#include "WifiManager.h"
#include "Config.h"
#include "ModoOperacion.h"
#include "ModoVinculacion.h"

// ---------------------------------------------------------------------------
// main.cpp: arranque del timbre. Decide UN modo en el boot y despacha su loop.
//
//   Modo::Operacion   -> ya vinculado: timbre funcionando (EventFlow + Screen).
//   Modo::Vinculacion -> sin configurar: portal web + BLE para recibir la red.
//
// ---------------------------------------------------------------------------

size_t getArduinoLoopTaskStackSize(void) { return Config::System::LOOP_TASK_STACK; }

enum class Modo
{
    Vinculacion,
    Operacion
};

static Modo modo = Modo::Vinculacion;

void seedWifiIfConfigured()
{
    if (!Config::Build::SEED_WIFI_ENABLED)
        return;

    Storage::saveWifiCredentials(Config::Build::SEED_SSID, Config::Build::SEED_PASSWORD);
    if (Config::Build::SEED_DEVICE_REGISTERED)
        Storage::saveDeviceRegistered(true);
    Serial.println("[APP] credenciales sembradas en NVS desde build flags");
}

void setup()
{
    Serial.begin(Config::System::SERIAL_BAUD);
    unsigned long start = millis();
    while (!Serial && (millis() - start < Config::System::SERIAL_WAIT_MS))
        delay(10);
    Serial.println("\nIniciando...");

    seedWifiIfConfigured();

    String ssid = Storage::loadWifiSSID();
    String password = Storage::loadWifiPassword();

    // Con credenciales que conectan -> modo operacion. Si no hay o fallan,
    // caemos a vinculacion para reconfigurar.
    if (ssid.length() > 0 && WifiManager::connect(ssid, password))
    {
        Serial.println("\nModo normal (sin BLE ni AP)");
        modo = Modo::Operacion;
        ModoOperacion::begin();
    }
    else
    {
        Serial.println("\nEntrando en modo configuracion");
        modo = Modo::Vinculacion;
        ModoVinculacion::begin(Device::getBleName());
    }
}

void loop()
{
    switch (modo)
    {
    case Modo::Operacion:
        ModoOperacion::loop();
        break;
    case Modo::Vinculacion:
        ModoVinculacion::loop();
        break;
    }
}
