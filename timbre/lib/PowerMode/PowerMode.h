#pragma once

#include <Arduino.h>

namespace PowerMode
{
    // Reposo: camara off, CPU a CPU_MHZ_IDLE, WiFi en modem sleep.
    void low();

    // Evento: CPU a CPU_MHZ_ACTIVE, WiFi sin sleep, camara on.
    void high();
}
