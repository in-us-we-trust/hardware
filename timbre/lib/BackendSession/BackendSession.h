#pragma once
#include <Arduino.h>
#include "MqttClient.h"    // para IaResponse
#include "MediaSource.h"   // fuente de video (RTSP)
#include "AudioSource.h"   // fuente de audio (RTSP)

// ---------------------------------------------------------------------------
// BackendSession: MECANISMO. Sabe COMO mandar y recibir, no decide QUE ni
// CUANDO. Expone primitivas que usa la capa de politica (EventFlow):
//   - enviarFoto(jpeg): abre la sesion publicando el press con la foto.
//   - iniciarMedia(): arranca el stream RTSP (video+audio) hacia MediaMTX.
//   - cerrarSesion(): frena el RTSP y publica stream_end.
//   - onRespuesta(cb): notifica cada ia_response valido para que EventFlow
//     decida.
//
// Es dueño del transporte hacia el backend: MQTT (press/stream_end/ia_response)
// Y el stream RTSP. EventFlow ya no habla con RtspPublisher directamente.
//
// No saca la foto de ninguna fuente: la recibe ya lista. Quien la obtiene es
// la capa de arriba.
// ---------------------------------------------------------------------------

namespace BackendSession
{
    // Callback para cada ia_response valido (de la sesion activa).
    typedef void (*RespuestaHandler)(const MqttClient::IaResponse &r);

    // Configura el transporte (MQTT + RTSP). NO conecta todavia: la conexion se
    // dispara con conectar(). Recibe las fuentes de media (camara y microfono)
    // que el stream RTSP va a publicar. Llamar una vez, en modo normal con WiFi.
    void initialize(const String &deviceId, MediaSource *video, AudioSource *audio);

    // Dispara la conexion al broker (no bloquea). Se resuelve en loop().
    void conectar();

    // true cuando el transporte esta listo para mandar/recibir (conectado y
    // suscripto). EventFlow espera esto antes de enviar el press.
    bool estaListo();

    // Registra el handler que recibe cada ia_response. Lo usa EventFlow.
    void onRespuesta(RespuestaHandler handler);

    // Abre la sesion: publica el press con la PRIMERA foto. El trackId lo
    // genera y es dueño EventFlow; aca lo recibimos, lo guardamos como sesion
    // activa (para filtrar ia_response) y lo usamos como eventId del press.
    // (JPEG crudo; se codifica a base64 adentro, detalle de transporte MQTT.)
    // Devuelve true si el press salio.
    bool enviarFoto(const String &trackId, const uint8_t *jpeg, size_t len);

    // Igual que enviarFoto pero recibe la foto YA codificada en base64 (para
    // una constante hardcodeada de prueba, sin pasar por la fuente).
    bool enviarFotoBase64(const String &trackId, const String &fotoBase64);

    // Manda una foto ADICIONAL de la sesion (capture_image) con el JPEG en
    // base64. Solo si hay sesion activa (press ya enviado) con ese trackId.
    // Devuelve true si el publish salio.
    bool enviarCaptura(const String &trackId, const uint8_t *jpeg, size_t len);

    // Arranca el stream RTSPS hacia MediaMTX en {deviceId}/doorbell/{trackId}.
    // Negocia todos los tracks con fuente, pero NO envia nada todavia.
    // Se abre DESPUES del press, solo si hay persona o sonido; al quedar ready
    // se avisa con avisarInicioMedia() (stream_start). El audio y el video van
    // por RTSP y el gateway los extrae en chunks. Devuelve true si arranco la
    // negociacion (TLS OK).
    bool iniciarMedia(const String &trackId);

    // true cuando MediaMTX acepto el RECORD (path ready para el PLAY del gateway).
    bool mediaLista();

    // true si la negociacion RTSP fallo (TLS, 401, SETUP...).
    bool mediaFallo();

    // Publica stream_start: el RTSP de trackId ya esta publicando y el gateway
    // puede hacer PLAY. Se llama cuando mediaLista() da true.
    void avisarInicioMedia(const String &trackId);

    // Frena el RTSP sin cerrar la sesion MQTT (no publica stream_end). Para
    // cuando la negociacion falla y se vuelve al analisis local.
    void detenerMedia();

    // Prende/apaga el envio de cada track EN VIVO (sin reconectar). Lo llama
    // EventFlow segun su deteccion: audio cuando hay sonido, video cuando el
    // tiny model ve una persona.
    void activarVideo(bool activo);
    void activarAudio(bool activo);

    // Cierra la sesion: frena el stream RTSP y publica stream_end con el
    // trackId que le pasa EventFlow.
    void cerrarSesion(const String &trackId);

    // Bombeo del transporte (MQTT + RTSP mientras publica). Llamar en cada
    // vuelta del loop() principal.
    void loop();

    bool haySesionActiva();
}
