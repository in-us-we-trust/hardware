#pragma once

#include <Arduino.h>

namespace CameraManager
{
    void begin();
    String captureImageBase64();
}