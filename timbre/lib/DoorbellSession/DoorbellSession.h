#pragma once
#include <Arduino.h>
#include "MqttClient.h" // para IaResponse

// ---------------------------------------------------------------------------
// DoorbellSession: MECANISMO. Sabe COMO mandar y recibir, no decide QUE ni
// CUANDO. Expone primitivas que usa la capa de politica (EventFlow):
//   - enviarFoto(jpeg): abre la sesion publicando el press con la foto.
//   - enviarVideo()/enviarAudio(): plano de media por RTSP (fase siguiente).
//   - cerrarSesion(): stream_end.
//   - onRespuesta(cb): notifica cada ia_response valido para que EventFlow
//     decida.
//
// No saca la foto de ninguna fuente: la recibe ya lista. Quien la obtiene es
// la capa de arriba.
// ---------------------------------------------------------------------------

namespace DoorbellSession
{
    // Callback para cada ia_response valido (de la sesion activa).
    typedef void (*RespuestaHandler)(const MqttClient::IaResponse &r);

    // Configura el transporte (MQTT). NO conecta todavia: la conexion se
    // dispara con conectar(). Llamar una vez, en modo normal con WiFi.
    void begin(const String &deviceId);

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

    // --- Placeholders: plano de media por RTSP (fase siguiente) ------------
    // El audio y el video NO se mandan como blobs: van por el stream RTSP y el
    // gateway los extrae en chunks (ExtractVideoChunk/ExtractAudioChunk). Estas
    // primitivas controlaran el stream cuando lo implementemos.
    void enviarVideo();
    void enviarAudio();

    // Cierra la sesion: publica stream_end con el trackId que le pasa EventFlow.
    void cerrarSesion(const String &trackId);

    // Bombeo MQTT. Llamar en cada vuelta del loop() principal.
    void loop();

    bool haySesionActiva();
}
