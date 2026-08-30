#include "DoorbellSession.h"

#include "Device.h"
#include "MqttClient.h"
#include "RtspPublisher.h"
#include "MockMediaSource.h"

// --- Config (desde platformio.ini via build_flags, con defaults) -----------
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "test.mosquitto.org"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif
#ifndef MEDIAMTX_HOST
// Host de MediaMTX alcanzable por el gateway. Ajustar al lab real.
#define MEDIAMTX_HOST "192.168.1.50"
#endif

namespace
{
    // Fuente de media para el publisher. Hoy mock (JPEG embebido); en fase C
    // se cambia por CameraMediaSource. A 10 fps.
    MockMediaSource mediaSource(10);

    String deviceId;
    String trackId;   // == eventId del press; "" cuando no hay sesion
    bool active = false;

    DoorbellSession::DangerHandler dangerHandler = nullptr;
    DoorbellSession::MessageHandler messageHandler = nullptr;

    // Se invoca con cada ia_response que MqttClient ya filtro por deviceId.
    // Aca aplicamos el filtro fino por trackId (llamadas solapadas / stale).
    void handleIaResponse(const MqttClient::IaResponse &r)
    {
        if (!active || r.trackId != trackId)
        {
            // Instruccion de una sesion que no es la activa: se descarta.
            Serial.printf("[SESSION] ia_response ignorado (track=%s, activo=%s)\n",
                          r.trackId.c_str(), trackId.c_str());
            return;
        }

        // Emitir el mensaje hacia arriba (Screen) para cualquier accion.
        if (messageHandler)
        {
            messageHandler(r.chunkType, r.action, r.severity, r.message);
        }

        if (r.isStopDanger())
        {
            Serial.printf("[SESSION] STOP_DANGER (sev=%s): cortando sesion %s\n",
                          r.severity.c_str(), trackId.c_str());
            // El gateway YA cerro la sesion de su lado: solo cortamos el media
            // y disparamos la alerta local. No mandamos stream_end.
            RtspPublisher::stop();
            active = false;
            String finished = trackId;
            trackId = "";
            if (dangerHandler)
            {
                dangerHandler(r.message);
            }
            (void)finished;
        }
        else
        {
            // CONTINUE: seguir publicando. No hace falta otro press.
            Serial.printf("[SESSION] CONTINUE (chunk=%s, sev=%s)\n",
                          r.chunkType.c_str(), r.severity.c_str());
        }
    }
}

namespace DoorbellSession
{
    void begin(const String &id)
    {
        deviceId = id;
        active = false;
        trackId = "";

        // Media: configurar el publisher (todavia no publica).
        RtspPublisher::begin(MEDIAMTX_HOST, &mediaSource);

        // Control: conectar MQTT y quedar escuchando ia_response.
        MqttClient::begin(MQTT_BROKER_HOST, MQTT_BROKER_PORT, deviceId);
        MqttClient::onIaResponse(handleIaResponse);

        Serial.println("[SESSION] lista (MQTT conectando, RTSP configurado)");
    }

    String startCall()
    {
        if (active)
        {
            // Contrato: un press nuevo para el mismo deviceId cancela la sesion
            // anterior. Reemplazamos con un trackId nuevo.
            Serial.printf("[SESSION] startCall con sesion activa (%s): se reemplaza\n",
                          trackId.c_str());
        }

        // 1) El stream RTSP tiene que estar listo ANTES del press. Si no,
        //    FFmpeg falla el DESCRIBE (404) y no hay VIDEO/AUDIO.
        if (!RtspPublisher::isPublishing())
        {
            if (!RtspPublisher::start())
            {
#ifdef DEV_RTSP_OPTIONAL
                // Fase de desarrollo (ej. Wokwi, sin MediaMTX alcanzable): no
                // abortamos, mandamos el press igual para validar el MQTT solo.
                Serial.println("[SESSION] RTSP no conecto (DEV_RTSP_OPTIONAL): sigo con el press");
#else
                Serial.println("[SESSION] no se pudo iniciar RTSP: press abortado");
                return "";
#endif
            }
        }

        // 2) eventId nuevo = trackId de toda la sesion.
        trackId = Device::generateUUID();
        String rtspUrl = RtspPublisher::streamUrl();

        // 3) press por MQTT. Sin imagen: si falta, el gateway saca un frame por
        //    RTSP (mantiene el payload chico). Bateria informativa opcional.
        bool ok = MqttClient::publishPress(trackId, rtspUrl);
        if (!ok)
        {
            Serial.println("[SESSION] press no se pudo publicar; corto RTSP");
            RtspPublisher::stop();
            trackId = "";
            return "";
        }

        active = true;
        Serial.printf("[SESSION] llamada iniciada trackId=%s\n", trackId.c_str());
        return trackId;
    }

    void endCall()
    {
        if (!active)
        {
            return;
        }
        Serial.printf("[SESSION] cerrando llamada trackId=%s\n", trackId.c_str());

        // Corte normal / timeout: cortar media y avisar con stream_end.
        RtspPublisher::stop();
        MqttClient::publishStreamEnd(trackId);
        // NOTA fase deep-sleep: antes de dormir hay que asegurar el flush del
        // PUBACK del stream_end (bombear MqttClient::loop()). Se agrega cuando
        // metamos el sleep.

        active = false;
        trackId = "";
    }

    void onDanger(DangerHandler handler)
    {
        dangerHandler = handler;
    }

    void onMessage(MessageHandler handler)
    {
        messageHandler = handler;
    }

    void loop()
    {
        MqttClient::loop();
        RtspPublisher::loop();
    }

    bool hasActiveSession()
    {
        return active;
    }

    String currentTrackId()
    {
        return trackId;
    }
}
