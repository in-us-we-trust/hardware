#pragma once
#include <Arduino.h>
#include "MediaSource.h"
#include "AudioSource.h"

// Host/puerto de MediaMTX viven en Config.h (Config::Media), con override por
// build flag en platformio.ini. El host se pasa ademas como parametro a initialize().

// ---------------------------------------------------------------------------
// RtspPublisher: publica video y/o audio hacia MediaMTX por RTSPS (push).
//
// Transporte: TLS contra MediaMTX (profile secure, :8322). El server cert se
// valida con CA_CERT contra Config::Media::TLS_HOSTNAME. MediaMTX no pide client
// cert: la identidad va por user/pass (Basic) en cada request, y solo deja
// publicar en paths que empiezan con {deviceId}/.
// Path por evento: {deviceId}/doorbell/{trackId} (lo arma BackendSession).
//
// Dos tracks RTP interleaved sobre la misma conexion TLS:
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

    void initialize(const String &mediaMtxHost, const String &user, const String &pass,
                    MediaSource *videoSource, AudioSource *audioSource = nullptr);

    // Arranca la publicacion en `path` NEGOCIANDO todos los tracks con fuente
    // disponible (SDP + SETUP), pero SIN enviar nada todavia. La negociacion
    // avanza en loop(); cuando state() == Publishing el path esta "ready" en
    // MediaMTX. Que se envia se prende/apaga con setVideoActivo()/setAudioActivo().
    // OJO: MediaMTX corta al publisher si no recibe datos en readTimeout (10 s).
    // Devuelve false si no hay fuentes o falla el TLS.
    bool start(const String &path);
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
