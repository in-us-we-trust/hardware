#include "EventFlow.h"

#include "DoorbellSession.h"
#include "Screen.h"
#include "Device.h"
#include "foto_prueba.h"

#ifndef EVENTFLOW_CONNECT_TIMEOUT_MS
#define EVENTFLOW_CONNECT_TIMEOUT_MS 10000 // 10s esperando la conexion
#endif

namespace
{
    enum class Estado
    {
        IDLE,       // en reposo
        CONECTANDO, // foto ya sacada, esperando que MQTT quede listo
        EN_SESION   // press enviado, escuchando ia_response
    };

    MediaSource *src = nullptr;
    Estado estado = Estado::IDLE;
    unsigned long conectandoDesde = 0;

    // Foto sacada al tocar el timbre, esperando a que la conexion este lista.
    // NOTA: hoy guardamos el puntero (el mock vive en flash, es estable). Con
    // camara real habra que COPIAR el frame, porque el framebuffer se reusa.
    const uint8_t *fotoPtr = nullptr;
    size_t fotoLen = 0;

    // trackId: identidad del evento. Nace y vive en EventFlow; se lo pasamos a
    // DoorbellSession para el press, el stream_end y el filtrado de ia_response.
    String trackId = "";

    void mostrarEnScreen(const MqttClient::IaResponse &r)
    {
        String text = "[" + r.chunkType + "] " + r.action;
        if (r.message.length() > 0)
            text += " \u2014 " + r.message;
        Screen::showMessage(text, r.severity);
    }

    // Cada ia_response de la sesion activa (via DoorbellSession).
    void onRespuesta(const MqttClient::IaResponse &r)
    {
        mostrarEnScreen(r); // 1) siempre reflejar en la pantalla

        // 2) decidir el siguiente paso
        if (r.isStopDanger())
        {
            Serial.println("[FLOW] STOP_DANGER -> cierro la sesion + alerta local");
            // TODO: alerta local (buzzer / luz)
            DoorbellSession::cerrarSesion(trackId);
            estado = Estado::IDLE;
        }
        else
        {
            Serial.println("[FLOW] CONTINUE -> (placeholder) aca sigue el flujo");
            // >>> PLACEHOLDER: despues de la foto (audio/video por RTSP) <<<
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
        estado = Estado::IDLE;
        Serial.println("[FLOW] EventFlow listo (IDLE)");
    }

    void onTimbre()
    {
        if (estado != Estado::IDLE)
        {
            Serial.println("[FLOW] timbre ignorado: ya hay un evento en curso");
            return;
        }
        Serial.println("[FLOW] timbre tocado");

        // El trackId nace aca: identifica todo el evento.
        trackId = Device::generateUUID();
        Serial.printf("[FLOW] trackId del evento: %s\n", trackId.c_str());

        // (A) Disparar la conexion MQTT -- no bloquea, avanza en loop().
        DoorbellSession::conectar();

        // (B) La foto: por ahora HARDCODEADA (constante base64). Cuando este la
        // camara real, aca volvera a salir de la fuente (src->nextVideoFrame).
        Serial.printf("[FLOW] foto de prueba lista (%u chars base64)\n",
                      (unsigned)strlen(FOTO_PRUEBA_B64));

        // (C) PLACEHOLDER: analisis local de la imagen (futuro). Corre mientras
        // la conexion se establece. Podria incluso decidir no abrir la sesion.

        conectandoDesde = millis();
        estado = Estado::CONECTANDO;
    }

    void terminar()
    {
        DoorbellSession::cerrarSesion(trackId);
        estado = Estado::IDLE;
    }

    void loop()
    {
        DoorbellSession::loop(); // bombea MQTT siempre (conexion + entrantes)

        if (estado == Estado::CONECTANDO)
        {
            if (DoorbellSession::estaListo())
            {
                // Conexion lista -> mandar el press con la foto hardcodeada.
                if (!DoorbellSession::enviarFotoBase64(trackId, FOTO_PRUEBA_B64))
                {
                    Serial.println("[FLOW] fallo al enviar el press: vuelvo a IDLE");
                    estado = Estado::IDLE;
                    return;
                }
                Serial.printf("[FLOW] sesion iniciada trackId=%s\n", trackId.c_str());
                estado = Estado::EN_SESION;
            }
            else if (millis() - conectandoDesde > EVENTFLOW_CONNECT_TIMEOUT_MS)
            {
                Serial.println("[FLOW] timeout conectando: aborto, vuelvo a IDLE");
                estado = Estado::IDLE;
            }
        }
    }
}
