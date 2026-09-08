#include "LowPower.h"
#include "CameraManager.h"
#include <WiFi.h>

namespace LowPower
{
    void enterIdleMode()
    {
        Serial.println("Entrando en modo de bajo consumo...");

        // Apagamos la camara para ahorrar energia
        CameraManager::end();

        // Habilitamos sleep de WiFi (modem sleep)
        WiFi.setSleep(true);

        // Reducimos la frecuencia de la CPU para bajar temperatura y consumo
        // Valores comunes: 240, 160, 80
        setCpuFrequencyMhz(80);

        Serial.println("CPU a 80MHz, WiFi en modo sleep, camara apagada.");
    }
}
