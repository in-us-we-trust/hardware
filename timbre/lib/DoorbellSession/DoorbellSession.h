#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// DoorbellSession: orquestador de la sesion de timbre. Es la UNICA capa que
// entiende el flujo del contrato (press -> ia_response -> stream_end, TTL,
// STOP_DANGER) y coordina los transportes por debajo: MqttClient (control) y
// RtspPublisher (media).
//
// El main NO deberia hablar con MqttClient ni RtspPublisher directamente: solo
// con esta capa. DoorbellSession, a su vez, NO maneja WiFi ni deep sleep: asume
// que ya hay red. Esa frontera es lo que la mantiene limpia.
//
//   main  --startCall()/endCall()/onDanger()-->  DoorbellSession
//   DoorbellSession  -->  MqttClient  +  RtspPublisher
// ---------------------------------------------------------------------------

namespace DoorbellSession
{
    // Callback cuando el gateway ordena cortar por peligro (STOP_DANGER).
    // Aca el main dispara la alerta local (buzzer, luz, lo que sea).
    typedef void (*DangerHandler)(const String &message);

    // Callback para CADA ia_response valido (de la sesion activa), sin importar
    // la accion. Lo usa el main para mostrarlo en la Screen. Strings sueltos
    // para no acoplar Screen/main al transporte MQTT.
    typedef void (*MessageHandler)(const String &chunkType, const String &action,
                                   const String &severity, const String &message);

    // Arranca los transportes: conecta MQTT (queda escuchando ia_response) y
    // configura el RtspPublisher. Llamar una vez, en modo normal con WiFi.
    // La config del broker y de MediaMTX viene de build flags (platformio.ini).
    void begin(const String &deviceId);

    // Dispara una llamada: deja el stream RTSP listo, publica el press y abre
    // la sesion. Devuelve el trackId (== eventId) de la sesion, o "" si fallo.
    // Un startCall con sesion activa reemplaza la anterior (trackId nuevo).
    String startCall();

    // Cierra la llamada activa "por corte normal / timeout": corta el RTSP y
    // publica stream_end con el trackId de la sesion. No hace nada si no hay
    // sesion activa.
    void endCall();

    // Registra el handler de peligro. Opcional.
    void onDanger(DangerHandler handler);

    // Registra el handler de mensajes (todo ia_response). Opcional.
    void onMessage(MessageHandler handler);

    // Bombeo: adentro llama a MqttClient::loop() y RtspPublisher::loop().
    // Imprescindible en cada vuelta del loop() principal.
    void loop();

    // true si hay una sesion de timbre en curso.
    bool hasActiveSession();

    // trackId de la sesion activa ("" si no hay).
    String currentTrackId();
}
