#pragma once
#include <Arduino.h>
#include "MediaSource.h"

class CameraSource : public MediaSource
{
public:
    explicit CameraSource(int fps = 5) : _fps(fps) {}

    // Enciende la camara (esp_camera_init). true si quedo lista.
    bool begin() override;

    // Apaga la camara (esp_camera_deinit) y libera la copia interna. Para reposo.
    void end();

    // Captura un frame nuevo y lo entrega. data apunta a un buffer INTERNO
    // propiedad de esta clase; NO liberar. Valido hasta la proxima captura.
    // Lo usan tanto el RTSP (interfaz MediaSource) como el stream local.
    bool nextVideoFrame(const uint8_t **data, size_t *len) override;

    // FPS objetivo para el video (marca la cadencia con la que el RtspPublisher
    // pide frames). El stream local no lo usa: captura on-demand por request.
    int videoFps() const override { return _fps; }

    // Foto para el press / provisioning BLE: captura fresca y la devuelve en
    // base64 (JPEG sin prefijo data:). "" si falla.
    String captureBase64();

    bool ready() const { return _ready; }

private:
    bool captureIntoBuffer(); // fb_get -> copia a _buf -> fb_return

    bool _ready = false;
    int _fps;
    uint8_t *_buf = nullptr; // copia del ultimo frame (PSRAM si hay)
    size_t _cap = 0;         // capacidad asignada de _buf
    size_t _len = 0;         // bytes validos del ultimo frame
};

// Instancia global: el unico dueño de la camara en todo el firmware.
extern CameraSource Camera;
