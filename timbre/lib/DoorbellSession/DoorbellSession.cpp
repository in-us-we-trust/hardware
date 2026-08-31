#include "DoorbellSession.h"

#include "Device.h"
#include "MqttClient.h"
#include "mbedtls/base64.h"

// --- Config (desde platformio.ini via build_flags, con defaults) -----------
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "test.mosquitto.org"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif
#ifndef MEDIAMTX_HOST
#define MEDIAMTX_HOST "192.168.1.50"
#endif
#ifndef MEDIAMTX_PORT
#define MEDIAMTX_PORT 8554
#endif
#ifndef MEDIAMTX_PATH
#define MEDIAMTX_PATH "doorbell"
#endif

namespace
{
    String deviceId;
    String trackId; // == eventId del press; "" cuando no hay sesion
    bool active = false;

    DoorbellSession::RespuestaHandler respuestaHandler = nullptr;

    String rtspUrl()
    {
        return String("rtsp://") + MEDIAMTX_HOST + ":" + String(MEDIAMTX_PORT) +
               "/" + MEDIAMTX_PATH;
    }

    // Codifica bytes crudos a base64 (para el campo image del press, MQTT).
    String toBase64(const uint8_t *data, size_t len)
    {
        size_t need = 0;
        mbedtls_base64_encode(nullptr, 0, &need, data, len); // pide tamaño
        if (need == 0) return "";
        uint8_t *out = (uint8_t *)malloc(need);
        if (!out) return "";
        size_t written = 0;
        String result;
        if (mbedtls_base64_encode(out, need, &written, data, len) == 0)
        {
            result = String((const char *)out); // mbedtls deja el \0
        }
        free(out);
        return result;
    }

    // Se invoca con cada ia_response ya filtrado por deviceId (MqttClient).
    // Aca filtramos por trackId y reenviamos a EventFlow.
    void handleIaResponse(const MqttClient::IaResponse &r)
    {
        if (!active || r.trackId != trackId)
        {
            Serial.printf("[SESSION] ia_response ignorado (track=%s, activo=%s)\n",
                          r.trackId.c_str(), trackId.c_str());
            return;
        }
        if (respuestaHandler)
        {
            respuestaHandler(r);
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
        MqttClient::begin(MQTT_BROKER_HOST, MQTT_BROKER_PORT, deviceId);
        MqttClient::onIaResponse(handleIaResponse);
        Serial.println("[SESSION] configurada (MQTT sin conectar aun)");
    }

    void onRespuesta(RespuestaHandler handler)
    {
        respuestaHandler = handler;
    }

    void conectar()
    {
        MqttClient::conectar();
    }

    bool estaListo()
    {
        return MqttClient::listo();
    }

    bool enviarFoto(const String &id, const uint8_t *jpeg, size_t len)
    {
        String fotoB64 = toBase64(jpeg, len);
        if (fotoB64.length() == 0)
        {
            Serial.println("[SESSION] no se pudo codificar la foto");
            return false;
        }
        return enviarFotoBase64(id, fotoB64);
    }

    bool enviarFotoBase64(const String &id, const String &fotoBase64)
    {
        // El trackId lo genera EventFlow; aca lo guardamos como sesion activa
        // (para filtrar ia_response) y lo usamos como eventId del press.
        trackId = id;
        bool ok = MqttClient::publishPress(trackId, rtspUrl(), fotoBase64);
        if (!ok)
        {
            Serial.println("[SESSION] no se pudo publicar el press");
            trackId = "";
            return false;
        }

        active = true;
        Serial.printf("[SESSION] press enviado (foto base64 %u chars) trackId=%s\n",
                      (unsigned)fotoBase64.length(), trackId.c_str());
        return true;
    }

    void enviarVideo()
    {
        // >>> FASE SIGUIENTE <<<
        // Aca ira el arranque del track de video en el stream RTSP, de donde el
        // gateway extrae los VideoChunk.
        Serial.println("[SESSION] enviarVideo() TODO (RTSP fase siguiente)");
    }

    void enviarAudio()
    {
        // >>> FASE SIGUIENTE <<<
        // Idem para el track de audio (AudioChunk del gateway).
        Serial.println("[SESSION] enviarAudio() TODO (RTSP fase siguiente)");
    }

    void cerrarSesion(const String &id)
    {
        if (!active) return;
        MqttClient::publishStreamEnd(id);
        Serial.printf("[SESSION] sesion cerrada (stream_end) trackId=%s\n",
                      id.c_str());
        active = false;
        trackId = "";
    }

    void loop()
    {
        MqttClient::loop();
    }

    bool haySesionActiva() { return active; }
}
