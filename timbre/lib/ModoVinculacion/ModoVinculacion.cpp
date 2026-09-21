#include "ModoVinculacion.h"

#include "Config.h"
#include "WebPortal.h"
#include "BleManager.h"
#include "CameraSource.h"

#include <Arduino.h>

namespace ModoVinculacion
{
    void begin(const String &bleName)
    {
        // Wokwi no simula camara ni BLE; en hardware real, si.
        if (!Config::Env::IS_WOKWI_SIM)
        {
            Camera.begin();
        }
        WebPortal::begin(bleName);
        if (!Config::Env::IS_WOKWI_SIM)
        {
            BleManager::begin(bleName);
        }
    }

    void loop()
    {
        WebPortal::handleClient();
        BleManager::loop(); // hace el trabajo pesado (WiFi + gateway) diferido desde BLE
    }
}
