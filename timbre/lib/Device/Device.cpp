#include "Device.h"
#include "Config.h"
#include "Certs.h"
#include "esp_system.h"
#include <string.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/oid.h>

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

    String getDeviceId()
    {
        return String(Config::Identity::ID);
    }

    bool certMatchesDeviceId()
    {
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);

        // mbedtls necesita el largo incluyendo el '\0' para PEM.
        int rc = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char *>(DEVICE_CERT),
                                        strlen(DEVICE_CERT) + 1);
        if (rc != 0)
        {
            Serial.printf("[Device] ERROR: DEVICE_CERT invalido (mbedtls -0x%04X)\n", -rc);
            mbedtls_x509_crt_free(&crt);
            return false;
        }

        String cn;
        for (const mbedtls_x509_name *n = &crt.subject; n != nullptr; n = n->next)
        {
            if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid) == 0)
            {
                cn.reserve(n->val.len);
                for (size_t i = 0; i < n->val.len; i++)
                    cn += static_cast<char>(n->val.p[i]);
                break;
            }
        }
        mbedtls_x509_crt_free(&crt);

        String id = getDeviceId();
        if (cn != id)
        {
            Serial.printf("[Device] ERROR: CN del cert (%s) != DEVICE_ID (%s). "
                          "El ACL del broker va a descartar los mensajes.\n",
                          cn.c_str(), id.c_str());
            return false;
        }
        Serial.printf("[Device] identidad OK: CN = DEVICE_ID = %s\n", id.c_str());
        return true;
    }

    String getBleName()
    {
        uint64_t chipid = ESP.getEfuseMac();
        char buffer[13];
        sprintf(buffer, "%012llX", chipid);
        return "TIMBRE-" + String(buffer).substring(6);
    }
}
