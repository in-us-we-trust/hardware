#include "EventFlow.h"

#include "DoorbellSession.h"
#include "Screen.h"
#include "Device.h"
#include "CameraSource.h"
#include "AudioSource.h"
#include "LowPower.h"
#include "RtspPublisher.h"

#include <WiFi.h>
#include <WiFiServer.h>

#ifndef EVENTFLOW_CONNECT_TIMEOUT_MS
#define EVENTFLOW_CONNECT_TIMEOUT_MS 10000
#endif

#define MJPEG_BUF_SIZE (100 * 1024)

// ---------------------------------------------------------------------------
// EventFlow: coordinador UNICO del evento.
//
// Dos caminos EN PARALELO al tocar el timbre:
//   LOCAL:   MJPEG task en core 0 (:8081), Screen en core 1 (:80)
//   BACKEND: MQTT press + RTSP video+audio hacia MediaMTX
// ---------------------------------------------------------------------------

namespace
{
    enum class Backend
    {
        OFF,
        CONECTANDO,
        EN_SESION
    };

    MediaSource *src = nullptr;

    bool eventoActivo = false;
    Backend backend = Backend::OFF;
    unsigned long conectandoDesde = 0;
    String trackId = "";

    volatile bool hangupPedido = false;

    // --- MJPEG task (core 0) ----------------------------------------------
    TaskHandle_t mjpegTaskHandle = nullptr;
    volatile bool mjpegStop = false;

    void mjpegTask(void *param)
    {
        WiFiServer mjpegServer(8081);
        mjpegServer.begin();
        Serial.println("[MJPEG] server iniciado en :8081 (core 0)");

        uint8_t *buf = (uint8_t *)ps_malloc(MJPEG_BUF_SIZE);
        if (!buf)
        {
            Serial.println("[MJPEG] sin memoria para buffer");
            mjpegServer.stop();
            vTaskDelete(NULL);
            return;
        }

        while (!mjpegStop)
        {
            WiFiClient client = mjpegServer.available();
            if (!client)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            Serial.println("[MJPEG] cliente conectado");

            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
            client.println("Access-Control-Allow-Origin: *");
            client.println("Cache-Control: no-cache");
            client.println("Connection: close");
            client.println();

            while (client.connected() && !mjpegStop)
            {
                size_t len = 0;
                if (Camera.captureFrameCopy(buf, MJPEG_BUF_SIZE, &len))
                {
                    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", len);
                    client.write(buf, len);
                    client.print("\r\n");
                }
                vTaskDelay(pdMS_TO_TICKS(40));
            }

            client.stop();
            Serial.println("[MJPEG] cliente desconectado");
        }

        free(buf);
        mjpegServer.stop();
        Serial.println("[MJPEG] task terminado");
        mjpegTaskHandle = nullptr;
        vTaskDelete(NULL);
    }

    void startMjpegTask()
    {
        mjpegStop = false;
        xTaskCreatePinnedToCore(mjpegTask, "mjpeg", 8192, NULL, 1,
                                &mjpegTaskHandle, 0);
    }

    void stopMjpegTask()
    {
        if (!mjpegTaskHandle)
            return;
        mjpegStop = true;
        for (int i = 0; i < 40 && mjpegTaskHandle != nullptr; i++)
            delay(50);
        if (mjpegTaskHandle)
        {
            vTaskDelete(mjpegTaskHandle);
            mjpegTaskHandle = nullptr;
        }
    }

    void raisePower()
    {
        setCpuFrequencyMhz(240);
        WiFi.setSleep(false);
    }

    void endEvent()
    {
        Serial.println("[FLOW] hangup local -> cierro el evento");

        if (backend != Backend::OFF || DoorbellSession::haySesionActiva())
            DoorbellSession::cerrarSesion(trackId);

        // Parar RTSP antes de apagar camara/mic
        if (RtspPublisher::isPublishing() || RtspPublisher::state() != RtspPublisher::State::Idle)
            RtspPublisher::stop();

        backend = Backend::OFF;

        stopMjpegTask();
        Microphone.end();
        Screen::clear();
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

    void onRespuesta(const MqttClient::IaResponse &r)
    {
        mostrarEnScreen(r);

        if (r.isStopDanger())
        {
            Serial.println("[FLOW] STOP_DANGER -> cierro SOLO el backend (el stream local sigue)");
            if (RtspPublisher::isPublishing())
                RtspPublisher::stop();
            DoorbellSession::cerrarSesion(trackId);
            backend = Backend::OFF;
        }
        else
        {
            Serial.println("[FLOW] CONTINUE -> backend sigue");
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
        DoorbellSession::begin(deviceId);
        DoorbellSession::onRespuesta(onRespuesta);
        Screen::onHangup(EventFlow::terminar);

        // Configurar RTSP publisher con Camera + Microphone
        RtspPublisher::begin(
            String(MEDIAMTX_HOST), // del platformio.ini
            &Camera,               // video: dueño unico de la camara
            &Microphone,           // audio: dueño unico del microfono
            "doorbell");

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

        raisePower();
        Camera.begin();
        Microphone.begin();
        startMjpegTask();
        hangupPedido = false;
        eventoActivo = true;

        DoorbellSession::conectar();
        conectandoDesde = millis();
        backend = Backend::CONECTANDO;
    }

    void terminar()
    {
        if (eventoActivo)
            hangupPedido = true;
    }

    void loop()
    {
        DoorbellSession::loop();

        // Bombear audio (lee del DMA, acumula, codifica chunks)
        if (eventoActivo)
        {
            Microphone.loop();

            // Empujar el nivel a la pantalla para la animacion (throttle ~120ms,
            // no cada chunk de 20ms, para no saturar el SSE ni la red).
            static unsigned long lastLevelMs = 0;
            if (millis() - lastLevelMs >= 120)
            {
                lastLevelMs = millis();
                Screen::pushLevel(Microphone.level());
            }
        }

        // Bombear RTSP (manda video + audio por RTP)
        if (backend == Backend::EN_SESION)
            RtspPublisher::loop();

        if (backend == Backend::CONECTANDO)
        {
            if (DoorbellSession::estaListo())
            {
                if (!DoorbellSession::enviarFotoBase64(trackId, Camera.captureBase64()))
                {
                    Serial.println("[FLOW] fallo el press: cierro backend (el evento local sigue)");
                    backend = Backend::OFF;
                }
                else
                {
                    Serial.printf("[FLOW] sesion de backend iniciada trackId=%s\n", trackId.c_str());
                    backend = Backend::EN_SESION;

                    // Arrancar RTSP (video + audio hacia MediaMTX)
                    if (!RtspPublisher::start())
                        Serial.println("[FLOW] RTSP no arranco (el evento local sigue)");
                }
            }
            else if (millis() - conectandoDesde > EVENTFLOW_CONNECT_TIMEOUT_MS)
            {
                Serial.println("[FLOW] timeout conectando backend: sigo solo con local");
                backend = Backend::OFF;
            }
        }

        if (hangupPedido)
        {
            hangupPedido = false;
            endEvent();
        }
    }
}
