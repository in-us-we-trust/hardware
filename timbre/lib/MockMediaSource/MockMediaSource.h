#pragma once
#include "MediaSource.h"

// Fuente mockeada: devuelve un JPEG embebido en loop, como si fuera la camara.
// Para desarrollar el pipeline RTSP sin OV2640 ni hardware.
class MockMediaSource : public MediaSource
{
public:
    explicit MockMediaSource(int fps = 10) : _fps(fps) {}

    bool begin() override;
    bool nextVideoFrame(const uint8_t **data, size_t *len) override;
    int videoFps() const override { return _fps; }

private:
    int _fps;
};
