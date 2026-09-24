#include "BackendSession.h"

#include "Device.h"
#include "MqttClient.h"
#include "RtspPublisher.h"
#include "Config.h"
#include "mbedtls/base64.h"

namespace
{
    String deviceId;
    String trackId; // == eventId del press; "" cuando no hay sesion
    bool active = false;

    BackendSession::RespuestaHandler respuestaHandler = nullptr;

    // Codifica bytes crudos a base64 (para el campo image del press, MQTT).
    String toBase64(const uint8_t *data, size_t len)
    {
        size_t need = 0;
        mbedtls_base64_encode(nullptr, 0, &need, data, len); // pide tamaño
        if (need == 0)
            return "";
        uint8_t *out = (uint8_t *)malloc(need);
        if (!out)
            return "";
        size_t written = 0;
        String result;
        if (mbedtls_base64_encode(out, need, &written, data, len) == 0)
        {
            result = String((const char *)out); // mbedtls deja el \0
        }
        free(out);
        return result;
    }

    // Se invoca con cada ia_response de este device (el topico ya es
    // home/{deviceId}/...). Aca filtramos por trackId y reenviamos a EventFlow.
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

namespace BackendSession
{
    void initialize(const String &id, MediaSource *video, AudioSource *audio)
    {
        deviceId = id;
        active = false;
        trackId = "";
        MqttClient::initialize(Config::Mqtt::BROKER_HOST, Config::Mqtt::BROKER_PORT, deviceId);
        // CALLBACK
        MqttClient::onIaResponse(handleIaResponse);

        // El stream RTSP (video + audio) tambien es transporte hacia el backend.
        RtspPublisher::initialize(Config::Media::HOST, Config::Media::USER, Config::Media::PASS,
                                  video, audio);

        Serial.println("[SESSION] configurada (MQTT + RTSP sin conectar aun)");
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
        bool ok = MqttClient::publishPress(trackId, fotoBase64);
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

    bool iniciarMedia(const String &id)
    {
        // Path que el gateway reconstruye: {RTSP_BASE}/{deviceId}/doorbell/{trackId}.
        String path = deviceId + "/" + Config::Media::PATH + "/" + id;
        // Negocia todos los tracks disponibles; el envio arranca apagado.
        bool ok = RtspPublisher::start(path);
        if (!ok)
            Serial.println("[SESSION] RTSP no arranco (el evento local sigue)");
        return ok;
    }

    bool mediaLista()
    {
        return RtspPublisher::isPublishing();
    }

    bool mediaFallo()
    {
        return RtspPublisher::state() == RtspPublisher::State::Error;
    }

    void activarVideo(bool activo)
    {
        RtspPublisher::setVideoActivo(activo);
    }

    void activarAudio(bool activo)
    {
        RtspPublisher::setAudioActivo(activo);
    }

    void cerrarSesion(const String &id)
    {
        // Frenar el RTSP siempre (aunque la sesion MQTT ya no este activa), para
        // no dejar el stream publicando.
        if (RtspPublisher::isPublishing() || RtspPublisher::state() != RtspPublisher::State::Idle)
            RtspPublisher::stop();

        if (!active)
            return;
        MqttClient::publishStreamEnd(id);
        Serial.printf("[SESSION] sesion cerrada (stream_end) trackId=%s\n",
                      id.c_str());
        active = false;
        trackId = "";
    }

    void loop()
    {
        MqttClient::loop();

        // Bombear el RTSP en TODOS los estados activos: loop() es el que avanza
        // ANNOUNCE -> SETUP -> RECORD y despues manda video+audio. (Antes solo se
        // llamaba en Publishing y la negociacion quedaba trabada en Announcing.)
        RtspPublisher::State s = RtspPublisher::state();
        if (s != RtspPublisher::State::Idle && s != RtspPublisher::State::Error)
            RtspPublisher::loop();
    }

    bool haySesionActiva() { return active; }
}
