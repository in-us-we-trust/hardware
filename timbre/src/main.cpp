#include <Arduino.h>
#include <WiFi.h>
#include "Storage.h"
#include "Device.h"
#include "WifiManager.h"
#include "WebPortal.h"
#include "BleManager.h"
#include "Certs.h"
#include "CameraManager.h"

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

    if (ssid.length() > 0) {
        Serial.println("Credenciales encontradas");
        if (WifiManager::connect(ssid, password)) {
            Serial.println("\nModo normal (sin BLE ni AP)");
            return;
        }
    }

    Serial.println("\nEntrando en modo configuracion");
    CameraManager::begin();
    WebPortal::begin(bleName);
    BleManager::begin(bleName);
}

void loop()
{
    WebPortal::handleClient();
    BleManager::loop(); // hace el trabajo pesado (WiFi + gateway) diferido desde BLE
}