#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// MqttClient: plano de control del timbre contra el gateway-microcontroller.
//
// Publica (el gateway escucha):
//   - home/doorbell/events/press          (inicio de llamada)          QoS 1
//   - home/doorbell/events/stream_end     (cierre de sesion)           QoS 1
//   - home/doorbell/events/capture_image  (captura on-demand, opc.)    QoS 1
//   - home/doorbell/events/capture_audio  (captura on-demand, opc.)    QoS 1
//
// Se suscribe (el gateway publica):
//   - home/doorbell/events/ia_response    (instruccion post-analisis)  QoS 1
//
// Reglas del contrato que respeta este modulo:
//   - QoS 1 y sin retain en todo.
//   - deviceId estable (Device::getOrCreateDeviceId), eventId nuevo por llamada.
//   - eventId del press == trackId de toda la sesion; vuelve en ia_response y va
//     en stream_end.
//   - El JSON es UTF-8 valido (ArduinoJson). No se manda media binaria por MQTT.
//
// El broker en el lab es plano (tcp://host:1883, sin TLS ni auth). El mTLS del
// proyecto es solo para el ApiClient HTTPS, no para MQTT.
// ---------------------------------------------------------------------------

namespace MqttClient
{
    // Instruccion recibida del gateway (ia_response ya parseada).
    struct IaResponse
    {
        String trackId;   // == eventId del press de esta sesion
        String deviceId;  // el de este dispositivo
        String chunkType; // "IMAGE" | "VIDEO" | "AUDIO"
        String action;    // "CONTINUE" | "STOP_DANGER"
        String severity;  // "NORMAL" | "LOW" | "MEDIUM" | "HIGH"
        String message;   // texto para UI / log / TTS

        bool isStopDanger() const { return action == "STOP_DANGER"; }
        bool isContinue() const { return action == "CONTINUE"; }
    };

    // Callback que se invoca por cada ia_response cuyo deviceId coincide con el
    // nuestro. El filtrado por trackId (llamadas solapadas) lo hace el llamador.
    typedef void (*IaResponseHandler)(const IaResponse &response);

    // Configura el cliente. deviceId se usa para filtrar ia_response y para
    // derivar un clientId unico ("timbre-<deviceId>"). No conecta todavia:
    // la conexion se maneja de forma perezosa en loop().
    //   host: IP/hostname del broker alcanzable por el ESP32 y el gateway.
    //   port: 1883 tipico.
    void begin(const String &host, uint16_t port, const String &deviceId);

    // Registra el handler de ia_response. Opcional.
    void onIaResponse(IaResponseHandler handler);

    // Bombea el cliente: mantiene la conexion, reintenta si se cayo, procesa
    // mensajes entrantes. Llamar en cada vuelta del loop() principal.
    void loop();

    // true si hay sesion MQTT activa con el broker.
    bool connected();

    // --- Publicaciones (todas QoS 1, sin retain) ---------------------------

    // press: inicio de llamada. El eventId ES el trackId de la sesion.
    //   rtspUrl: URL que el gateway puede reproducir (rtsp:// o rtsps://). No
    //            puede ser 127.0.0.1 si el gateway corre en otro host.
    //   imageBase64: JPEG en Base64. Opcional; si se omite el gateway saca un
    //                frame por RTSP. Recomendado dejarlo vacio para no inflar
    //                el payload (y subir el buffer si se decide mandarlo).
    //   batteryPercentage: informativo; NAN para omitirlo.
    // Devuelve true si el publish salio (token OK). No confirma entrega end-to-end.
    bool publishPress(const String &eventId,
                      const String &rtspUrl,
                      const String &imageBase64 = "",
                      float batteryPercentage = NAN);

    // stream_end: el ESP32 deja de publicar RTSP. trackId == eventId del press.
    bool publishStreamEnd(const String &trackId);

    // Captura on-demand (opcional, no es el flujo de timbre). Si hay sesion
    // activa se pueden omitir trackId y rtspUrl; si no, hacen falta ambos.
    bool publishCaptureImage(const String &trackId = "", const String &rtspUrl = "");
    bool publishCaptureAudio(const String &trackId = "", const String &rtspUrl = "");
}
