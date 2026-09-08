#include "Device.h"
#include "Storage.h"
#include "esp_system.h"

namespace Device
{
    String generateUUID()
    {
        uint8_t uuid[16];
        esp_fill_random(uuid, sizeof(uuid));

        // UUID v4: version bits y variant bits
        uuid[6] = (uuid[6] & 0x0F) | 0x40;
        uuid[8] = (uuid[8] & 0x3F) | 0x80;

        char uuidStr[37];
        snprintf(uuidStr, sizeof(uuidStr),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid[0], uuid[1], uuid[2], uuid[3],
                 uuid[4], uuid[5],
                 uuid[6], uuid[7],
                 uuid[8], uuid[9],
                 uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        return String(uuidStr);
    }

    // Genera el UUID la primera vez y lo persiste en flash.
    // Reinicios posteriores reusan el mismo ID.
    String getOrCreateDeviceId()
    {
        String id = Storage::loadDeviceId();
        if (id.isEmpty())
        {
            id = generateUUID();
            Storage::saveDeviceId(id);
            Serial.println("UUID generado: " + id);
        }
        else
        {
            Serial.println("UUID existente: " + id);
        }
        return id;
    }

    String getBleName()
    {
        uint64_t chipid = ESP.getEfuseMac();
        char buffer[13];
        sprintf(buffer, "%012llX", chipid);
        return "TIMBRE-" + String(buffer).substring(6);
    }
}
