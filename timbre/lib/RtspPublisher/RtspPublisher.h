#pragma once
#include <Arduino.h>
#include "MediaSource.h"
#include "AudioSource.h"

// Host/puerto de MediaMTX viven en Config.h (Config::Media), con override por
// build flag en platformio.ini. El host se pasa ademas como parametro a initialize().

// ---------------------------------------------------------------------------
// RtspPublisher: publica video y/o audio hacia MediaMTX por RTSP (push).
//
// Dos tracks RTP interleaved sobre la misma conexion TCP:
//   Track 0 (video): JPEG → RTP-JPEG (RFC 2435), canales 0-1
//   Track 1 (audio): G.711 u-law → RTP PCMU (payload 0), canales 2-3
//
// Que tracks se publican se elige por evento en start(sendVideo, sendAudio).
// Un track se activa solo si ademas se le paso su fuente en initialize()
// (sin AudioSource no hay audio aunque se pida; idem video).
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

    void initialize(const String &mediaMtxHost, MediaSource *videoSource,
               AudioSource *audioSource = nullptr,
               const String &path = "doorbell");

    // Arranca la publicacion NEGOCIANDO todos los tracks con fuente disponible
    // (los declara en el SDP y hace el SETUP), pero SIN enviar nada todavia.
    // Que se envia realmente se prende/apaga en vivo con setVideoActivo() /
    // setAudioActivo(). Devuelve false si no hay ninguna fuente.
    bool start();
    void stop();

    // Prende/apaga el ENVIO de cada track en vivo, sin renegociar la sesion.
    // Solo tiene efecto si el track fue negociado (su fuente existe). Esto es
    // lo que usa EventFlow para decidir "ahora mando video/audio, ahora no".
    void setVideoActivo(bool activo);
    void setAudioActivo(bool activo);
    bool isPublishing();
    String streamUrl();
    void loop();
    State state();
}
