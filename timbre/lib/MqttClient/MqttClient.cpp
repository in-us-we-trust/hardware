#include "MqttClient.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <MQTT.h>        // 256dpi/arduino-mqtt
#include <ArduinoJson.h> // bblanchon/ArduinoJson
#include <time.h>

#include "Certs.h"  // CA_CERT, DEVICE_CERT, DEVICE_KEY (identidad mTLS)
#include "Config.h" // Config::Mqtt::TLS_HOSTNAME

// ---------------------------------------------------------------------------
// Config por build flags (con defaults). Se pueden pisar desde platformio.ini.
// ---------------------------------------------------------------------------
#ifndef MQTT_BUFFER_SIZE
// press/stream_end son JSON chicos (<1KB). Con la imagen base64 en el press
// hace falta mas (una QVGA base64 ronda 15-35 KB).
#define MQTT_BUFFER_SIZE 2048
#endif

#ifndef MQTT_KEEPALIVE_SECONDS
#define MQTT_KEEPALIVE_SECONDS 30
#endif

#ifndef MQTT_RECONNECT_INTERVAL_MS
#define MQTT_RECONNECT_INTERVAL_MS 3000
#endif

// Timeout de CONNACK/SUBACK. Con TLS contra una VM en internet 2 s quedaba justo.
#ifndef MQTT_CONNECT_TIMEOUT_MS
#define MQTT_CONNECT_TIMEOUT_MS 5000
#endif

// Timeout del handshake TLS, en SEGUNDOS (WiFiClientSecure::setHandshakeTimeout).
#ifndef MQTT_TLS_HANDSHAKE_TIMEOUT_S
#define MQTT_TLS_HANDSHAKE_TIMEOUT_S 10
#endif

// Cuantos intentos fallidos seguidos antes de abandonar el backend (para no
// trabar el stream local). Se reintenta en el proximo conectar().
#ifndef MQTT_MAX_CONNECT_ATTEMPTS
#define MQTT_MAX_CONNECT_ATTEMPTS 2
#endif

namespace
{
    const char *ACTION_PRESS = "press";
    const char *ACTION_STREAM_END = "stream_end";
    const char *ACTION_CAPTURE_IMAGE = "capture_image";
    const char *ACTION_CAPTURE_AUDIO = "capture_audio";
    const char *ACTION_IA_RESPONSE = "ia_response";

    WiFiClientSecure net;
    MQTTClient mqtt(MQTT_BUFFER_SIZE);

    String brokerHost;
    uint16_t brokerPort = 8883;
    String deviceId;
    String clientId;
    String topicIaResponse; // home/{deviceId}/events/+/ia_response

    MqttClient::IaResponseHandler iaHandler = nullptr;

    bool started = false;    // configurado (initialize llamado)
    bool enabled = false;    // debe estar conectado (conectar llamado)
    bool subscribed = false; // suscripto a ia_response
    unsigned long lastConnectAttempt = 0;
    int connectAttempts = 0; // fallos seguidos; al llegar al max, abandona

    // Ultimos messageId vistos, para descartar reentregas QoS 1.
    constexpr size_t SEEN_IDS = 8;
    String seenIds[SEEN_IDS];
    size_t seenNext = 0;

    bool yaVisto(const String &messageId)
    {
        if (messageId.length() == 0)
            return false;
        for (size_t i = 0; i < SEEN_IDS; i++)
        {
            if (seenIds[i] == messageId)
                return true;
        }
        seenIds[seenNext] = messageId;
        seenNext = (seenNext + 1) % SEEN_IDS;
        return false;
    }

    // home/{deviceId}/events/{trackId}/{action}
    String topicFor(const String &trackId, const char *action)
    {
        return "home/" + deviceId + "/events/" + trackId + "/" + action;
    }

    // Parsea home/{deviceId}/events/{trackId}/{action}. Devuelve false si el
    // topico no tiene esa forma o no es de este device.
    bool parseTopic(const String &topic, String &trackId, String &action)
    {
        String prefix = "home/" + deviceId + "/events/";
        if (!topic.startsWith(prefix))
            return false;
        int slash = topic.indexOf('/', prefix.length());
        if (slash < 0)
            return false;
        trackId = topic.substring(prefix.length(), slash);
        action = topic.substring(slash + 1);
        return trackId.length() > 0 && action.length() > 0 && action.indexOf('/') < 0;
    }

    // Epoch en milisegundos UTC, o 0 si el reloj todavia no esta sincronizado.
    int64_t nowEpochMillis()
    {
        time_t now = time(nullptr);
        if (now < 1600000000L) // ~2020-09; antes de eso el reloj no arranco
            return 0;
        return static_cast<int64_t>(now) * 1000LL;
    }

    void addTimestamp(JsonDocument &doc)
    {
        int64_t ts = nowEpochMillis();
        if (ts > 0)
            doc["timestamp"] = ts;
    }

    void handleIaResponse(const String &trackId, const String &payload)
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (err)
        {
            Serial.printf("[MQTT] ia_response JSON invalido: %s\n", err.c_str());
            return;
        }

        MqttClient::IaResponse r;
        r.trackId = trackId;
        r.messageId = doc["messageId"] | "";
        r.chunkType = doc["chunkType"] | "";
        r.action = doc["action"] | "";
        r.severity = doc["severity"] | "";
        r.message = doc["message"] | "";

        if (yaVisto(r.messageId))
        {
            Serial.printf("[MQTT] ia_response duplicado (messageId=%s), ignorado\n",
                          r.messageId.c_str());
            return;
        }

        Serial.printf("[MQTT] <- ia_response track=%s chunk=%s action=%s sev=%s\n",
                      r.trackId.c_str(), r.chunkType.c_str(),
                      r.action.c_str(), r.severity.c_str());

        if (iaHandler)
            iaHandler(r);
    }

    // Callback de arduino-mqtt (firma simple: topic + payload).
    void onMessage(String &topic, String &payload)
    {
        String trackId, action;
        if (!parseTopic(topic, trackId, action))
        {
            Serial.printf("[MQTT] topico inesperado: %s\n", topic.c_str());
            return;
        }
        if (action == ACTION_IA_RESPONSE)
            handleIaResponse(trackId, payload);
    }

    bool publishJson(const String &topic, JsonDocument &doc)
    {
        if (!mqtt.connected())
        {
            Serial.printf("[MQTT] publish a %s descartado: sin conexion\n", topic.c_str());
            return false;
        }

        String payload;
        serializeJson(doc, payload);

        // arduino-mqtt: publish(topic, payload, retained=false, qos=1)
        bool ok = mqtt.publish(topic, payload, false, 1);
        if (!ok)
        {
            Serial.printf("[MQTT] publish a %s fallo (err=%d)\n",
                          topic.c_str(), mqtt.lastError());
        }
        return ok;
    }

    void onConnectFailed()
    {
        net.stop();
        connectAttempts++;
        if (connectAttempts >= MQTT_MAX_CONNECT_ATTEMPTS)
        {
            // Broker inalcanzable: dejamos de reintentar para no trabar el
            // stream local. Se reintenta en el proximo conectar() (timbre).
            enabled = false;
            Serial.println("[MQTT] broker inalcanzable, abandono el backend "
                           "(el stream local sigue)");
        }
    }

    void connectNow()
    {
        if (WiFi.status() != WL_CONNECTED)
            return; // sin WiFi no hay nada que hacer

        lastConnectAttempt = millis();
        Serial.printf("[MQTT] conectando a mqtts://%s:%u (tls-name=%s) como %s... (intento %d/%d)\n",
                      brokerHost.c_str(), brokerPort, Config::Mqtt::TLS_HOSTNAME,
                      clientId.c_str(), connectAttempts + 1, MQTT_MAX_CONNECT_ATTEMPTS);

        IPAddress ip;
        if (!WiFi.hostByName(brokerHost.c_str(), ip))
        {
            Serial.printf("[MQTT] no se pudo resolver %s\n", brokerHost.c_str());
            onConnectFailed();
            return;
        }

        // 1) TCP + handshake mTLS a mano. Se conecta a la IP pero se valida el
        //    server cert contra TLS_HOSTNAME (ver Config.h).
        net.stop();
        if (!net.connect(ip, brokerPort, Config::Mqtt::TLS_HOSTNAME,
                         CA_CERT, DEVICE_CERT, DEVICE_KEY))
        {
            char err[128] = {0};
            int code = net.lastError(err, sizeof(err));
            Serial.printf("[MQTT] handshake TLS fallo (%d): %s\n", code, err);
            onConnectFailed();
            return;
        }

        // 2) CONNECT MQTT sobre el socket ya abierto (skip=true: no reconecta).
        if (!mqtt.connect(clientId.c_str(), true))
        {
            // returnCode 5 = not authorized (CN rechazado por el broker).
            Serial.printf("[MQTT] CONNECT fallo (err=%d, ret=%d)\n",
                          mqtt.lastError(), mqtt.returnCode());
            onConnectFailed();
            return;
        }

        connectAttempts = 0;
        Serial.println("[MQTT] conectado (mTLS)");

        subscribed = mqtt.subscribe(topicIaResponse, 1);
        if (subscribed)
            Serial.printf("[MQTT] suscripto a %s (QoS 1)\n", topicIaResponse.c_str());
        else
            Serial.printf("[MQTT] suscripcion a %s RECHAZADA (err=%d). "
                          "Revisar ACL / CN del cert\n",
                          topicIaResponse.c_str(), mqtt.lastError());
    }
}

namespace MqttClient
{
    void initialize(const String &host, uint16_t port, const String &id)
    {
        brokerHost = host;
        brokerPort = port;
        deviceId = id;
        clientId = "timbre-" + id; // unico por dispositivo; nunca "gateway-micro"
        topicIaResponse = "home/" + deviceId + "/events/+/" + ACTION_IA_RESPONSE;

        net.setHandshakeTimeout(MQTT_TLS_HANDSHAKE_TIMEOUT_S);

        // begin() guarda host/puerto, pero la conexion de red la hacemos a mano
        // en connectNow() (mqtt.connect(..., skip=true)).
        mqtt.begin(brokerHost.c_str(), brokerPort, net);
        // cleanSession=true: no queremos cola persistente del lado del broker.
        mqtt.setOptions(MQTT_KEEPALIVE_SECONDS, true, MQTT_CONNECT_TIMEOUT_MS);
        mqtt.onMessage(onMessage);

        started = true;
        enabled = false; // no conecta hasta que se llame a conectar()
    }

    void onIaResponse(IaResponseHandler handler)
    {
        iaHandler = handler;
    }

    void loop()
    {
        if (!started)
            return;

        mqtt.loop();

        if (!mqtt.connected())
            subscribed = false; // si se cayo, ya no estamos suscriptos

        if (enabled && !mqtt.connected() &&
            (millis() - lastConnectAttempt) > MQTT_RECONNECT_INTERVAL_MS)
        {
            connectNow();
        }
    }

    bool connected()
    {
        return started && mqtt.connected();
    }

    void conectar()
    {
        if (!started)
            return;
        enabled = true;
        connectAttempts = 0; // cada evento arranca con los intentos frescos
        if (!mqtt.connected())
            connectNow();
    }

    void desconectar()
    {
        enabled = false;
        connectAttempts = 0;
        subscribed = false;
        if (mqtt.connected())
            mqtt.disconnect();
        net.stop();
    }

    bool listo()
    {
        return enabled && mqtt.connected() && subscribed;
    }

    bool publishPress(const String &trackId,
                      const String &imageBase64,
                      float batteryPercentage)
    {
        JsonDocument doc;
        doc["event"] = "press";
        addTimestamp(doc);
        if (imageBase64.length() > 0)
            doc["image"] = imageBase64;
        if (!isnan(batteryPercentage))
            doc["batteryPercentage"] = batteryPercentage;

        String topic = topicFor(trackId, ACTION_PRESS);
        Serial.printf("[MQTT] -> %s (imagen %u chars)\n",
                      topic.c_str(), (unsigned)imageBase64.length());
        return publishJson(topic, doc);
    }

    bool publishStreamEnd(const String &trackId)
    {
        JsonDocument doc;
        addTimestamp(doc);
        String topic = topicFor(trackId, ACTION_STREAM_END);
        Serial.printf("[MQTT] -> %s\n", topic.c_str());
        return publishJson(topic, doc);
    }

    bool publishCaptureImage(const String &trackId)
    {
        JsonDocument doc;
        addTimestamp(doc);
        return publishJson(topicFor(trackId, ACTION_CAPTURE_IMAGE), doc);
    }

    bool publishCaptureAudio(const String &trackId)
    {
        JsonDocument doc;
        addTimestamp(doc);
        return publishJson(topicFor(trackId, ACTION_CAPTURE_AUDIO), doc);
    }
}
