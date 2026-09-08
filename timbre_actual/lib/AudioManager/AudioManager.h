#pragma once

#include <Arduino.h>

namespace AudioManager
{
    void begin();
    void beginInput();
    void beginOutput();
    void end();
    void loop();
    bool isReady();
}
