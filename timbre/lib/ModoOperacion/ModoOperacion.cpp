#include "ModoOperacion.h"

#include "Config.h"
#include "Device.h"
#include "Screen.h"
#include "EventFlow.h"
#include "PowerMode.h"
#include "MockMediaSource.h"
#include "PersonDetector.h"

#include <Arduino.h>

namespace
{
    // Fuente de la foto. Hoy mock (JPEG embebido); manana CameraMediaSource.
    MockMediaSource fotoSource(1);

    String deviceId;

    // --- Boton fisico (timbre) ----------------------------------------------
    // El ISR SOLO marca la bandera; el trabajo real (arrancar el evento) lo
    // hace loop() en la tarea principal. onButton corre en contexto de
    // interrupcion, por eso va en IRAM y no toca nada pesado.
    volatile bool buttonPressed = false;
    volatile uint32_t lastButtonMs = 0;

    void IRAM_ATTR onButton()
    {
        uint32_t now = millis();
        if (now - lastButtonMs < Config::Button::DEBOUNCE_MS)
            return; // ignora rebotes del pulsador
        lastButtonMs = now;
        buttonPressed = true; // EventFlow::onTimbre ignora el disparo si ya hay evento
    }

    // Disparo de prueba por serial (reemplaza al boton en Wokwi).
    void handleSerialTrigger()
    {
        if (!Serial.available())
            return;
        char c = Serial.read();
        if (c == 'p')
            EventFlow::onTimbre();
        else if (c == 'e')
            EventFlow::terminar();
    }
}

namespace ModoOperacion
{
    void begin()
    {
        deviceId = Device::getOrCreateDeviceId();
        Serial.printf("[APP] deviceId = %s\n", deviceId.c_str());
        Serial.println("[APP] serial: 'p' = timbrar, 'e' = colgar (local)");

        // Boton fisico (no existe en Wokwi, pero attachInterrupt es inocuo alli).
        pinMode(Config::Button::PIN, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(Config::Button::PIN), onButton, FALLING);

        Screen::begin();                              // Visor
        EventFlow::initialize(deviceId, &fotoSource); // Lo inicializa. Pero no comienza hasta onTimbre
        PersonDetector::begin();                      // TFLite + tensor arena en PSRAM

        // Arrancamos en reposo: camara off, CPU 80MHz, WiFi modem sleep.
        PowerMode::low();
    }

    void loop()
    {
        EventFlow::loop();      // backend + stream local
        Screen::handleClient(); // atiende la pagina de estado (SSE)
        handleSerialTrigger();

        if (buttonPressed)
        {
            buttonPressed = false;
            EventFlow::onTimbre(); // ignora el disparo si ya hay un evento
        }
    }
}
