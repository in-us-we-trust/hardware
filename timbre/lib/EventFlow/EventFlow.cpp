#include "EventFlow.h"

#include "DoorbellSession.h"
#include "Screen.h"
#include "Device.h"
#include "CameraSource.h"
#include "LowPower.h"
#include "foto_prueba.h"

#include <WiFi.h>
#include <WebServer.h>

#ifndef EVENTFLOW_CONNECT_TIMEOUT_MS
#define EVENTFLOW_CONNECT_TIMEOUT_MS 10000 // 10s esperando la conexion al backend
#endif

// ---------------------------------------------------------------------------
// EventFlow: coordinador UNICO del evento (fusion de la ex politica de backend
// + el stream local que antes vivia en CallManager). Al tocar el timbre se
// disparan DOS caminos EN PARALELO:
//
//   onTimbre()
//     |-- LOCAL  (dueño del ciclo de vida): sube energia, prende camara y
//     |          levanta el server HTTP :8081 (/stream, /hangup). El colgado
//     |          SIEMPRE es local: /hangup es la UNICA salida del evento ->
//     |          cierra todo + LowPower::enterIdleMode().
//     '-- BACKEND (invitado): conecta MQTT y manda el press. Si llega
//                STOP_DANGER se cierra SOLO la sesion de backend (cerrarSesion);
//                la camara, la energia y el stream local NO se tocan.
//
// La camara la maneja SIEMPRE la instancia global Camera (CameraSource): tanto
// el stream local como (mas adelante) el RTSP le piden frames a ella.
// ---------------------------------------------------------------------------

namespace
{
    enum class Backend
    {
        OFF,        // camino de backend inactivo (o ya cerrado por STOP_DANGER)
        CONECTANDO, // foto lista, esperando que MQTT quede listo
        EN_SESION   // press enviado, escuchando ia_response
    };

    MediaSource *src = nullptr;

    bool eventoActivo = false;         // ciclo de vida del evento (lo posee LOCAL)
    Backend backend = Backend::OFF;    // sub-estado del camino de backend
    unsigned long conectandoDesde = 0;
    String trackId = "";

    WebServer *localServer = nullptr;  // stream local (ex CallManager, :8081)
    volatile bool hangupPedido = false; // lo setea /hangup o terminar()

    // --- server local (stream de video para ver en la LAN) ----------------
    void handleRoot()
    {
        localServer->sendHeader("Access-Control-Allow-Origin", "*");
        localServer->send(200, "text/html",
                          "<html><body><h2>Timbre</h2>"
                          "<p>Stream: <a href='/stream'>/stream</a></p>"
                          "<p><a href='/hangup'>Hangup</a></p></body></html>");
    }

    void handleHangup()
    {
        localServer->sendHeader("Access-Control-Allow-Origin", "*");
        localServer->send(200, "text/plain", "OK");
        hangupPedido = true; // el teardown real lo hace loop() -> endEvent()
    }

    void handleStream()
    {
        WiFiClient client = localServer->client();
        if (!client)
            return;

        const uint8_t *frame = nullptr;
        size_t frameLen = 0;
        if (!Camera.nextVideoFrame(&frame, &frameLen))
        {
            localServer->send(503, "text/plain", "No frame available");
            return;
        }

        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: image/jpeg");
        client.println("Access-Control-Allow-Origin: *");
        client.println("Cache-Control: no-cache, no-store, max-age=0");
        client.println("Connection: close");
        client.print("Content-Length: ");
        client.println(String(frameLen));
        client.println();
        client.write(frame, frameLen);
        client.stop();
        // el buffer es propiedad de CameraSource; NO se libera aca.
    }

    void startLocalServer()
    {
        localServer = new WebServer(8081);
        localServer->on("/", handleRoot);
        localServer->on("/hangup", handleHangup);
        localServer->on("/stream", handleStream);
        localServer->begin();
        Serial.println("[FLOW] stream local iniciado en :8081");
    }

    void stopLocalServer()
    {
        if (!localServer)
            return;
        localServer->stop();
        delete localServer;
        localServer = nullptr;
    }

    void raisePower()
    {
        setCpuFrequencyMhz(240);
        WiFi.setSleep(false);
    }

    // Fin del evento. SOLO se llega aca por /hangup local (o terminar()).
    void endEvent()
    {
        Serial.println("[FLOW] hangup local -> cierro el evento");

        // Si el backend seguia vivo, lo cerramos limpio.
        if (backend != Backend::OFF || DoorbellSession::haySesionActiva())
            DoorbellSession::cerrarSesion(trackId);
        backend = Backend::OFF;

        stopLocalServer();
        eventoActivo = false;

        LowPower::enterIdleMode(); // camara off + 80MHz + WiFi sleep
    }

    void mostrarEnScreen(const MqttClient::IaResponse &r)
    {
        String text = "[" + r.chunkType + "] " + r.action;
        if (r.message.length() > 0)
            text += " \u2014 " + r.message;
        Screen::showMessage(text, r.severity);
    }

    // Cada ia_response de la sesion de backend activa (via DoorbellSession).
    void onRespuesta(const MqttClient::IaResponse &r)
    {
        mostrarEnScreen(r); // reflejar siempre en la pantalla (SSE)

        if (r.isStopDanger())
        {
            Serial.println("[FLOW] STOP_DANGER -> cierro SOLO el backend (el stream local sigue)");
            // TODO: alerta local (buzzer / luz)
            DoorbellSession::cerrarSesion(trackId);
            backend = Backend::OFF; // muere el backend; el evento local continua
        }
        else
        {
            Serial.println("[FLOW] CONTINUE -> (placeholder) aca sigue el flujo de media");
            // >>> PLACEHOLDER: audio/video por RTSP <<<
        }
    }
}

namespace EventFlow
{
    void begin(const String &deviceId, MediaSource *source)
    {
        src = source;
        if (src)
            src->begin();
        DoorbellSession::begin(deviceId); // configura el transporte (no conecta)
        DoorbellSession::onRespuesta(onRespuesta);
        eventoActivo = false;
        backend = Backend::OFF;
        Serial.println("[FLOW] EventFlow listo (IDLE)");
    }

    void onTimbre()
    {
        if (eventoActivo)
        {
            Serial.println("[FLOW] timbre ignorado: ya hay un evento en curso");
            return;
        }
        Serial.println("[FLOW] timbre tocado");

        trackId = Device::generateUUID();
        Serial.printf("[FLOW] trackId del evento: %s\n", trackId.c_str());

        // --- LOCAL: dueño del evento. Sube energia, prende camara, server. ---
        raisePower();
        Camera.begin();
        startLocalServer();
        hangupPedido = false;
        eventoActivo = true;

        // --- BACKEND (invitado): dispara la conexion MQTT (no bloquea). ---
        DoorbellSession::conectar();
        conectandoDesde = millis();
        backend = Backend::CONECTANDO;
    }

    void terminar()
    {
        // Equivale a un hangup local (para el disparo por serial 'e').
        if (eventoActivo)
            hangupPedido = true;
    }

    void loop()
    {
        DoorbellSession::loop(); // bombea MQTT (conexion + entrantes)

        if (localServer)
            localServer->handleClient(); // atiende /stream y /hangup

        // Avanzar el camino de backend (solo mientras este vivo).
        if (backend == Backend::CONECTANDO)
        {
            if (DoorbellSession::estaListo())
            {
                if (!DoorbellSession::enviarFotoBase64(trackId, FOTO_PRUEBA_B64))
                {
                    Serial.println("[FLOW] fallo el press: cierro backend (el evento local sigue)");
                    backend = Backend::OFF;
                }
                else
                {
                    Serial.printf("[FLOW] sesion de backend iniciada trackId=%s\n", trackId.c_str());
                    backend = Backend::EN_SESION;
                }
            }
            else if (millis() - conectandoDesde > EVENTFLOW_CONNECT_TIMEOUT_MS)
            {
                Serial.println("[FLOW] timeout conectando backend: sigo solo con local");
                backend = Backend::OFF;
            }
        }

        // Salida del evento: SIEMPRE por hangup local.
        if (hangupPedido)
        {
            hangupPedido = false;
            endEvent();
        }
    }
}
