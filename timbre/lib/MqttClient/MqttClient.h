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
//
// ===========================================================================
// PAYLOADS EXACTOS (lo que sale del cable, tal cual lo arma este modulo)
// ===========================================================================
// Todos los payloads son un unico objeto JSON UTF-8, sin espacios ni saltos
// (serializeJson compacto), publicado con QoS 1 y retain=false.
//
// Campos comunes:
//   deviceId   string  - Device::getOrCreateDeviceId(). Siempre presente.
//   timestamp  int64   - epoch en MILISEGUNDOS UTC. SE OMITE si el reloj no
//                        esta sincronizado por NTP (time(nullptr) < 2020-09).
//                        O sea: el gateway no puede asumir que exista.
//
// --- home/doorbell/events/press --------------------------------------------
// Lo publica publishPress(). Campos:
//   eventId            string  - id de esta llamada; ES el trackId de la sesion.
//   deviceId           string
//   event              string  - literal constante "press".
//   rtspUrl            string  - siempre presente (puede venir vacio si el
//                                llamador pasa ""); no se valida aca.
//   timestamp          int64   - opcional (ver arriba).
//   image              string  - opcional; JPEG en Base64 SIN prefijo data:.
//                                Solo si imageBase64 != "". Ojo MQTT_BUFFER_SIZE.
//   batteryPercentage  float   - opcional; solo si no es NAN.
//
// {"eventId":"evt-7f3a","deviceId":"esp32-a1b2c3","event":"press",
//  "rtspUrl":"rtsp://192.168.0.42:8554/timbre","timestamp":1756512000000,
//  "batteryPercentage":87.5}
//
// --- home/doorbell/events/stream_end ---------------------------------------
// Lo publica publishStreamEnd(). Campos:
//   trackId    string  - == eventId del press de esta sesion.
//   deviceId   string
//   timestamp  int64   - opcional.
// NO lleva "event" ni "rtspUrl".
//
// {"trackId":"evt-7f3a","deviceId":"esp32-a1b2c3","timestamp":1756512045000}
//
// --- home/doorbell/events/capture_image ------------------------------------
// --- home/doorbell/events/capture_audio ------------------------------------
// Los publican publishCaptureImage() / publishCaptureAudio(). Mismo shape los
// dos, cambia solo el topic. Campos:
//   deviceId   string  - unico obligatorio.
//   trackId    string  - opcional; solo si se paso != "".
//   rtspUrl    string  - opcional; solo si se paso != "".
//   timestamp  int64   - opcional.
// NO llevan "event". El payload minimo posible es {"deviceId":"..."}.
//
// {"deviceId":"esp32-a1b2c3","trackId":"evt-7f3a",
//  "rtspUrl":"rtsp://192.168.0.42:8554/timbre","timestamp":1756512010000}
//
// --- home/doorbell/events/ia_response (ENTRADA, no lo publicamos) ----------
// Se espera un objeto con: trackId, deviceId, chunkType, action, severity,
// message (todos string). Los que falten quedan como "" en IaResponse. Si el
// deviceId viene y no es el nuestro, el mensaje se descarta sin llamar al
// handler.
//
// {"trackId":"evt-7f3a","deviceId":"esp32-a1b2c3","chunkType":"IMAGE",
//  "action":"CONTINUE","severity":"NORMAL","message":"Persona conocida"}
//
// Nota: si no hay conexion al broker, el publish se descarta (se loguea y
// devuelve false); no hay cola local ni reintento del mensaje.
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

    // true si hay sesion MQTT activa con el broker (TCP+CONNECT).
    bool connected();

    // Dispara la conexion al broker (no bloquea). El resultado se resuelve en
    // loop(). Antes de esto, begin() solo configura pero no conecta.
    void conectar();

    // true cuando esta LISTO para publicar/recibir: conectado Y suscripto a
    // ia_response. Usar esto (no connected()) antes de mandar el press.
    bool listo();

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
