#pragma once

#include <Arduino.h>

namespace CallManager
{
    void beginCall();
    void endCall();
    void handle(); // llamar periódicamente desde loop()
    bool inCall();
    bool isHangupRequested();
}
