#pragma once
#include <Arduino.h>
#include "MediaSource.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------------------
// CameraSource: dueño UNICO de la camara (OV2640). Nadie mas toca esp_camera.
//
// Thread-safe: un mutex protege el buffer interno para que el task de MJPEG
// (core 0) y el loop principal (core 1) no se pisen al capturar.
// ---------------------------------------------------------------------------
class CameraSource : public MediaSource
{
public:
    explicit CameraSource(int fps = 5);

    bool begin() override;
    void end();

    // Captura un frame y devuelve puntero al buffer INTERNO (propiedad de esta
    // clase). Valido hasta la proxima captura. Thread-safe (mutex).
    bool nextVideoFrame(const uint8_t **data, size_t *len) override;

    int videoFps() const override { return _fps; }

    // Captura un frame y COPIA al buffer del caller. Thread-safe. Pensado para
    // el task MJPEG que necesita escribir al socket (lento) sin retener el mutex.
    bool captureFrameCopy(uint8_t *dest, size_t destCap, size_t *outLen);

    // Foto para el press / provisioning BLE (base64). Thread-safe.
    String captureBase64();

    bool ready() const { return _ready; }

private:
    bool captureIntoBuffer(); // fb_get -> copia a _buf -> fb_return (NO toma mutex)

    bool _ready = false;
    int _fps;
    uint8_t *_buf = nullptr;
    size_t _cap = 0;
    size_t _len = 0;
    SemaphoreHandle_t _mutex;
};

extern CameraSource Camera;
