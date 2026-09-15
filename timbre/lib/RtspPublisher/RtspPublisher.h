#pragma once
#include <Arduino.h>
#include "MediaSource.h"
#include "AudioSource.h"

#ifndef MEDIAMTX_HOST
#define MEDIAMTX_HOST "192.168.1.69"
#endif
#ifndef MEDIAMTX_PORT
#define MEDIAMTX_PORT 8554
#endif

// ---------------------------------------------------------------------------
// RtspPublisher: publica video + audio hacia MediaMTX por RTSP (push).
//
// Dos tracks RTP interleaved sobre la misma conexion TCP:
//   Track 0 (video): JPEG → RTP-JPEG (RFC 2435), canales 0-1
//   Track 1 (audio): G.711 u-law → RTP PCMU (payload 0), canales 2-3
//
// El audio es opcional: si no se pasa AudioSource, solo publica video.
// ---------------------------------------------------------------------------

namespace RtspPublisher
{
    enum class State
    {
        Idle,
        Connecting,
        Announcing,
        SettingUpVideo,
        SettingUpAudio,
        Recording,
        Publishing,
        Error
    };

    void begin(const String &mediaMtxHost, MediaSource *videoSource,
               AudioSource *audioSource = nullptr,
               const String &path = "doorbell");

    bool start();
    void stop();
    bool isPublishing();
    String streamUrl();
    void loop();
    State state();
}
