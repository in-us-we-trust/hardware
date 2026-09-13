#include "LowPower.h"
#include "CameraSource.h"
#include <WiFi.h>

namespace LowPower
{
    void enterIdleMode()
    {
        Serial.println("Entrando en modo de bajo consumo...");

        // Apagamos la camara para ahorrar energia (unico dueño: CameraSource)
        Camera.end();

        // Habilitamos sleep de WiFi (modem sleep, sigue conectado)
        WiFi.setSleep(true);

        // Reducimos la frecuencia de la CPU para bajar temperatura y consumo
        setCpuFrequencyMhz(80);

        Serial.println("CPU a 80MHz, WiFi en modo sleep, camara apagada.");
    }
}
