#pragma once
#include <Arduino.h>

namespace Screen
{
    void begin();
    void handleClient();
    void stop();
    void showMessage(const String &text, const String &severity = "NORMAL");
    void clear();

    // Registra el callback de hangup. EventFlow lo llama con terminar().
    // El endpoint /hangup se agrega al server de Screen (:80).
    void onHangup(void (*callback)());
}
