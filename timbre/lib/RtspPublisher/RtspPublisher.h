#pragma once
#include <Arduino.h>
#include "MediaSource.h"

// ---------------------------------------------------------------------------
// RtspPublisher: publica el live del timbre hacia MediaMTX por RTSP (push).
// El ESP32 se conecta a MediaMTX y le EMPUJA el stream (ANNOUNCE/RECORD); el
// gateway despues hace PLAY a MediaMTX. Nunca al reves (NAT: el gateway remoto
// no puede entrar al ESP32).
//
// Transporte: RTP interleaved sobre la misma conexion TCP del RTSP (framing
// '$' + canal + longitud). Evita abrir puertos UDP y matchea el "tcp" del
// gateway.
//
// Los frames NO salen de la camara directamente: salen de una MediaSource
// (mock o camara real), inyectada en begin().
//
// >>> ESTADO: ESQUELETO <<<
// Handshake RTSP (OPTIONS/ANNOUNCE/SETUP/RECORD) + maquina de estados + cadencia
// de envio ya estructurados. El empaquetado RTP-JPEG (RFC 2435) esta marcado
// como el proximo bloque a completar (ver sendVideoFrame / packetizeJpeg).
// ---------------------------------------------------------------------------

namespace RtspPublisher
{
    enum class State
    {
        Idle,        // sin conexion
        Connecting,  // abriendo TCP
        Announcing,  // ANNOUNCE enviado, esperando 200
        SettingUp,   // SETUP enviado, esperando 200 (+ Session)
        Recording,   // RECORD enviado, esperando 200
        Publishing,  // enviando RTP
        Error
    };

    // Configura destino y fuente. No conecta todavia.
    //   mediaMtxHost: host alcanzable por el gateway.
    //   source:       fuente de frames (MockMediaSource, luego camara).
    //   path:         convencion de lab "doorbell".
    void begin(const String &mediaMtxHost, MediaSource *source,
               const String &path = "doorbell");

    // Inicia conexion + handshake. Devuelve true si arranco el intento (el
    // resultado real se resuelve asincronico en loop()).
    bool start();

    // TEARDOWN + cierre de la conexion + unpublish.
    void stop();

    // true si esta efectivamente publicando RTP (State::Publishing).
    bool isPublishing();

    // URL que el gateway puede reproducir: rtsp://<host>:<port>/<path>
    String streamUrl();

    // Bombeo: avanza el handshake y, en Publishing, envia frames a la cadencia
    // de la fuente. Llamar en cada vuelta del loop() principal.
    void loop();

    State state();
}
