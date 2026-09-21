#include "PowerMode.h"
#include "CameraSource.h"
#include "Config.h"
#include <WiFi.h>

namespace PowerMode
{
    void low()
    {
        Serial.println("Entrando en modo de bajo consumo...");

        // Apagamos la camara para ahorrar energia (unico dueño: CameraSource)
        Camera.end();

        // Habilitamos sleep de WiFi (modem sleep, sigue conectado)
        WiFi.setSleep(true);

        // Reducimos la frecuencia de la CPU para bajar temperatura y consumo
        setCpuFrequencyMhz(Config::System::CPU_MHZ_IDLE);

        Serial.printf("CPU a %uMHz, WiFi en modo sleep, camara apagada.\n", Config::System::CPU_MHZ_IDLE);
    }

    void high()
    {
        Serial.println("Entrando en modo de alto rendimiento...");

        // Primero CPU a full para que el resto arranque rapido
        setCpuFrequencyMhz(Config::System::CPU_MHZ_ACTIVE);

        // Sin modem sleep: baja latencia para MJPEG/RTSP
        WiFi.setSleep(false);

        // Prendemos la camara (unico dueño: CameraSource)
        Camera.begin();

        Serial.printf("CPU a %uMHz, WiFi sin sleep, camara encendida.\n", Config::System::CPU_MHZ_ACTIVE);
    }
}
