#include "EventFlow.h"

#include "DoorbellSession.h"
#include "Screen.h"
#include "Device.h"
#include "CameraSource.h"
#include "LowPower.h"
// foto_prueba.h ya no se usa: el press manda la captura real de Camera

#include <WiFi.h>
#include <WiFiServer.h>

#ifndef EVENTFLOW_CONNECT_TIMEOUT_MS
#define EVENTFLOW_CONNECT_TIMEOUT_MS 10000
#endif

// Buffer del task MJPEG: VGA JPEG cabe holgado en 100KB.
#define MJPEG_BUF_SIZE (100 * 1024)

// ---------------------------------------------------------------------------
// EventFlow: coordinador UNICO del evento.
//
// Dos caminos EN PARALELO al tocar el timbre:
//   LOCAL  (dueño del ciclo de vida):
//     - Sube energia, prende camara.
//     - Levanta un task MJPEG en core 0 que sirve el stream en :8081.
//     - /hangup vive en Screen (:80) y es la UNICA salida del evento.
//   BACKEND (invitado):
//     - Conecta MQTT y manda el press. Si llega STOP_DANGER se cierra SOLO
//       la sesion de backend; camara, energia y stream local NO se tocan.
//
// El stream MJPEG corre en un FreeRTOS task en core 0 para no bloquear el
// loop principal (core 1) que bombea MQTT, backend y Screen.
// ---------------------------------------------------------------------------

namespace
{
    enum class Backend { OFF, CONECTANDO, EN_SESION };

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

        // Buffer local: el task copia el frame aca antes de escribir al socket,
        // asi no retiene el mutex de Camera durante el I/O (lento).
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

            // Mandar header multipart (MJPEG estandar)
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
                vTaskDelay(pdMS_TO_TICKS(40)); // ~25 fps objetivo
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
        xTaskCreatePinnedToCore(
            mjpegTask,       // funcion
            "mjpeg",         // nombre
            8192,            // stack (bytes)
            NULL,            // param
            1,               // prioridad (baja, no critico)
            &mjpegTaskHandle,
            0                // core 0 (el loop de Arduino corre en core 1)
        );
    }

    void stopMjpegTask()
    {
        if (!mjpegTaskHandle)
            return;
        mjpegStop = true;
        // Esperamos a que el task termine limpiamente (max 2s).
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
        backend = Backend::OFF;

        stopMjpegTask();
        Screen::clear();
        eventoActivo = false;

        LowPower::enterIdleMode();
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
            DoorbellSession::cerrarSesion(trackId);
            backend = Backend::OFF;
        }
        else
        {
            Serial.println("[FLOW] CONTINUE -> (placeholder) aca sigue el flujo de media");
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

        // /hangup vive en Screen (:80) y dispara terminar().
        Screen::onHangup(EventFlow::terminar);

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
