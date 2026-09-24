#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// MqttClient: plano de control del timbre contra el gateway-microcontroller.
// Contrato: gateway-microcontroller/docs/contrato-esp32.md
//
// Transporte: MQTTS con mTLS (Mosquitto, profile "secure", puerto 8883).
//   - El timbre presenta DEVICE_CERT/DEVICE_KEY (lib/Certs). Mosquitto usa el
//     CN del cert como username (use_identity_as_username) y el ACL solo deja
//     tocar home/<CN>/#. Por eso deviceId == CN (ver Device::certMatchesDeviceId).
//   - El server cert se valida contra CA_CERT y contra el nombre
//     Config::Mqtt::TLS_HOSTNAME (ver Config.h: por que no la IP).
//
// Topicos (la identidad va SOLO en el topico, nunca en el JSON):
//   Publica (QoS 1, sin retain):
//     home/{deviceId}/events/{trackId}/press
//     home/{deviceId}/events/{trackId}/stream_end
//     home/{deviceId}/events/{trackId}/capture_image   (opcional)
//     home/{deviceId}/events/{trackId}/capture_audio   (opcional)
//   Se suscribe (QoS 1):
//     home/{deviceId}/events/+/ia_response
//     (wildcard SOLO en el trackId de este device; el trackId se saca del
//      topico y BackendSession filtra por la sesion activa)
//
// Payloads (JSON compacto UTF-8):
//   press:       {"event":"press","timestamp":<ms>,"image":"<b64>","batteryPercentage":<f>}
//                timestamp/image/batteryPercentage son opcionales.
//   stream_end:  {"timestamp":<ms>}
//   capture_*:   {"timestamp":<ms>}
//   ia_response (entrada):
//                {"messageId":"<uuid>","chunkType":"IMAGE|VIDEO|AUDIO",
//                 "action":"CONTINUE|STOP_DANGER","severity":"...","message":"..."}
//                Un messageId repetido es reentrega QoS 1: se descarta.
//
// timestamp = epoch ms UTC; se omite si el reloj no esta sincronizado (NTP).
// Si no hay conexion al broker, el publish se descarta (se loguea y devuelve
// false); no hay cola local ni reintento del mensaje.
// ---------------------------------------------------------------------------

namespace MqttClient
{
    // Instruccion recibida del gateway (ia_response ya parseada).
    struct IaResponse
    {
        String trackId;   // sacado del topico (== eventId del press)
        String messageId; // unico por publicacion del gateway
        String chunkType; // "IMAGE" | "VIDEO" | "AUDIO"
        String action;    // "CONTINUE" | "STOP_DANGER"
        String severity;  // finalSeverity del orquestador (p. ej. "PELIGROSO")
        String message;   // texto para UI / log / TTS

        bool isStopDanger() const { return action == "STOP_DANGER"; }
        bool isContinue() const { return action == "CONTINUE"; }
    };

    // Callback por cada ia_response de ESTE device. El filtrado por trackId
    // (sesion activa) lo hace el llamador.
    typedef void (*IaResponseHandler)(const IaResponse &response);

    // Configura el cliente. deviceId arma los topicos y el clientId
    // ("timbre-<deviceId>"). No conecta todavia: eso lo dispara conectar().
    //   host: IP o hostname del broker. port: 8883 (MQTTS).
    void initialize(const String &host, uint16_t port, const String &deviceId);

    // Registra el handler de ia_response. Opcional.
    void onIaResponse(IaResponseHandler handler);

    // Bombea el cliente: mantiene la conexion, reintenta si se cayo, procesa
    // mensajes entrantes. Llamar en cada vuelta del loop() principal.
    void loop();

    // true si hay sesion MQTT activa con el broker (TLS+CONNECT).
    bool connected();

    // Dispara la conexion al broker. OJO: el handshake TLS es bloqueante
    // (~1-3 s con RSA 2048); el stream local corre en el otro core.
    void conectar();

    // Deja de reintentar y cierra la conexion si estaba abierta. Tambien se
    // auto-invoca tras MQTT_MAX_CONNECT_ATTEMPTS fallos seguidos.
    void desconectar();

    // true cuando esta LISTO para publicar/recibir: conectado Y suscripto a
    // ia_response. Usar esto (no connected()) antes de mandar el press.
    bool listo();

    // --- Publicaciones (todas QoS 1, sin retain) ---------------------------

    // press: inicio de llamada. trackId (== eventId) lo genera EventFlow.
    //   imageBase64: JPEG en Base64 (opcional; si falta, el gateway saca un
    //                frame por RTSP). Ojo MQTT_BUFFER_SIZE.
    //   batteryPercentage: informativo; NAN para omitirlo.
    // Devuelve true si el publish salio. No confirma entrega end-to-end.
    bool publishPress(const String &trackId,
                      const String &imageBase64 = "",
                      float batteryPercentage = NAN);

    // stream_end: cierre de la llamada trackId.
    bool publishStreamEnd(const String &trackId);

    // Captura on-demand (opcional, no es el flujo de timbre).
    bool publishCaptureImage(const String &trackId);
    bool publishCaptureAudio(const String &trackId);
}
