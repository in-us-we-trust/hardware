#include "EventFlow.h"

#include "BackendSession.h"
#include "Screen.h"
#include "Device.h"
#include "CameraSource.h"
#include "AudioSource.h"
#include "PowerMode.h"
#include "Config.h"
#include "PersonDetector.h"

#include <WiFi.h>
#include <WiFiServer.h>

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
        OFF,        // sin sesion de backend: con evento activo, re-evalua cada
                    // EVAL_INTERVAL_MS si hay motivo (sonido/persona) para conectar
        EN_SESION   // press enviado, publicando media
    };

    bool eventoActivo = false;
    Backend backend = Backend::OFF;
    unsigned long ultimaEvaluacionMs = 0;
    String trackId = "";

    // Pico del nivel de microfono desde la ultima vez que se evaluo. level() es
    // solo el RMS del ultimo chunk (20ms): si se lee cada 1s, un sonido corto
    // entre chequeos se pierde. loop() va acumulando el maximo y haySonido()
    // lo consume.
    uint16_t picoAudio = 0;

    // Que ya se detecto en este intento de sesion. Se fijan al disparar la
    // conexion; en EN_SESION solo se chequea lo que sigue en false.
    bool audioDetectado = false;
    bool videoDetectado = false;

    // Foto del evento (JPEG, PSRAM). onTimbre la saca apenas se toca el timbre;
    // hayPersonaEnFrame() analiza esa primero y despues va capturando frames
    // nuevos. El press manda la ultima foto analizada.
    uint8_t *foto = nullptr;
    size_t fotoLen = 0;
    bool fotoSinAnalizar = false; // la foto del timbre todavia no paso por el modelo

    bool capturarFoto()
    {
        if (!foto)
            foto = (uint8_t *)ps_malloc(Config::Event::FOTO_BUF_SIZE);
        if (!foto)
        {
            Serial.println("[FLOW] sin memoria para la foto");
            fotoLen = 0;
            return false;
        }
        if (!Camera.captureFrameCopy(foto, Config::Event::FOTO_BUF_SIZE, &fotoLen))
        {
            Serial.println("[FLOW] no se pudo capturar la foto");
            fotoLen = 0;
            return false;
        }
        return true;
    }

    volatile bool hangupPedido = false;

    // --- MJPEG task (core 0) ----------------------------------------------
    TaskHandle_t mjpegTaskHandle = nullptr;
    volatile bool mjpegStop = false;

    void mjpegTask(void *param)
    {
        WiFiServer mjpegServer(Config::Mjpeg::PORT);
        mjpegServer.begin();
        Serial.printf("[MJPEG] server iniciado en :%u (core 0)\n", Config::Mjpeg::PORT);

        uint8_t *buf = (uint8_t *)ps_malloc(Config::Mjpeg::BUF_SIZE);
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
                vTaskDelay(pdMS_TO_TICKS(Config::Mjpeg::ACCEPT_IDLE_MS));
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
                if (Camera.captureFrameCopy(buf, Config::Mjpeg::BUF_SIZE, &len))
                {
                    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", len);
                    client.write(buf, len);
                    client.print("\r\n");
                }
                vTaskDelay(pdMS_TO_TICKS(Config::Mjpeg::FRAME_DELAY_MS));
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
        xTaskCreatePinnedToCore(mjpegTask, "mjpeg", Config::Mjpeg::TASK_STACK, NULL, 1,
                                &mjpegTaskHandle, Config::Mjpeg::TASK_CORE);
    }

    void stopMjpegTask()
    {
        if (!mjpegTaskHandle)
            return;
        mjpegStop = true;
        for (int i = 0; i < Config::Mjpeg::STOP_WAIT_TICKS && mjpegTaskHandle != nullptr; i++)
            delay(Config::Mjpeg::STOP_WAIT_MS);
        if (mjpegTaskHandle)
        {
            vTaskDelete(mjpegTaskHandle);
            mjpegTaskHandle = nullptr;
        }
    }

    void endEvent()
    {
        Serial.println("[FLOW] hangup local -> cierro el evento");
        Screen::showMessage("Finalizando llamada...");
        // cerrarSesion frena el RTSP y publica stream_end (ambos los maneja
        // BackendSession).
        if (backend != Backend::OFF || BackendSession::haySesionActiva())
            BackendSession::cerrarSesion(trackId);

        backend = Backend::OFF;
        audioDetectado = false;
        videoDetectado = false;
        fotoSinAnalizar = false;
        fotoLen = 0;

        stopMjpegTask();
        Microphone.end();
        Screen::clear();
        eventoActivo = false;

        PowerMode::low(); // camara off + 80MHz + WiFi sleep
    }

    // VIDEO: corre el tiny model (PersonDetector) sobre la foto del timbre si
    // todavia no se analizo; si no, sobre un frame nuevo.
    bool hayPersonaEnFrame()
    {
        if (fotoSinAnalizar)
            fotoSinAnalizar = false;
        else if (!capturarFoto())
            return false;

        if (fotoLen == 0)
            return false;
        return PersonDetector::detectJpeg(foto, fotoLen);
    }

    // AUDIO: hay sonido si el pico del microfono desde el ultimo chequeo supera
    // el umbral. El umbral se ajusta en Config.
    bool haySonido()
    {
        uint16_t pico = picoAudio;
        picoAudio = 0;
        return pico >= Config::Event::AUDIO_ACTIVATE_LEVEL;
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
            BackendSession::cerrarSesion(trackId); // frena RTSP + stream_end
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
    void initialize(const String &deviceId)
    {
        // BackendSession es dueño del transporte (MQTT + RTSP). Le pasamos las
        // fuentes de media que el stream RTSP va a publicar.
        BackendSession::initialize(deviceId, &Camera, &Microphone);

        // pasa los callbacks
        BackendSession::onRespuesta(onRespuesta);
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

        PowerMode::high(); // CPU full + WiFi sin sleep + camara on
        Microphone.begin();

        // Foto del timbre: se saca ya y es la primera que analiza el tiny model.
        fotoSinAnalizar = capturarFoto();

        // Alimenta el visor que ve el usuario. Es un task que funciona en el 2do core
        startMjpegTask();
        hangupPedido = false;
        eventoActivo = true;

        // El stream local ya corre. NO conectamos al backend todavia: con el
        // backend en OFF, loop() re-chequea cada EVAL_INTERVAL_MS si hay motivo
        // (sonido o persona) para conectar. El primer chequeo cae un intervalo
        // despues, lo que le da warm-up a la camara y el microfono.
        backend = Backend::OFF;
        ultimaEvaluacionMs = millis();
        picoAudio = 0;
    }

    void terminar()
    {
        if (eventoActivo)
            hangupPedido = true;
    }

    void loop()
    {
        BackendSession::loop();

        if (eventoActivo)
        {
            Microphone.loop();
            if (Microphone.level() > picoAudio)
                picoAudio = Microphone.level();

            // Empujar el nivel a la pantalla para la animacion (throttle ~120ms,
            // no cada chunk de 20ms, para no saturar el SSE ni la red).
            static unsigned long lastLevelMs = 0;
            if (millis() - lastLevelMs >= Config::Event::AUDIO_LEVEL_THROTTLE_MS)
            {
                lastLevelMs = millis();
                Screen::pushLevel(Microphone.level());
            }
        }

        // Evento activo sin backend (recien tocado, timeout, fallo del press o
        // STOP_DANGER): cada EVAL_INTERVAL_MS analiza frame y audio; con una
        // sola deteccion positiva, conecta. Si nunca hay motivo, se queda solo
        // con el stream local hasta el hangup.
        if (eventoActivo && backend == Backend::OFF)
        {
            if (millis() - ultimaEvaluacionMs >= Config::Event::EVAL_INTERVAL_MS)
            {
                ultimaEvaluacionMs = millis();
                // Evaluamos las dos (sin cortocircuito) para avisar cada deteccion en la Screen
                bool sonido = haySonido();
                bool persona = hayPersonaEnFrame();

                if (sonido && persona)
                    Screen::showMessage("Persona y audio detectada en el timbre. Analizando analisis inteligente...");
                else if (sonido)
                    Screen::showMessage("Audio entrante detectada en el timbre");
                else if (persona)
                    Screen::showMessage("Persona detectada en el timbre");
                if (sonido || persona)
                {
                    // Lo detectado queda prendido toda la sesion; en EN_SESION
                    // solo se sigue chequeando lo que falta.
                    audioDetectado = sonido;
                    videoDetectado = persona;
                    // BackendSession::conectar();
                    backend = Backend::EN_SESION;
                }
                else
                {
                    Screen::showMessage("No se detecta presencia en el timbre");
                }
            }
        }

        // En sesion, lo ya detectado no se re-analiza: solo se chequea lo que
        // falta y, al detectarlo, se prende y queda prendido.
        if (backend == Backend::EN_SESION)
        {
            static unsigned long lastAudioDetectMs = 0;
            if (!audioDetectado &&
                millis() - lastAudioDetectMs >= Config::Event::AUDIO_DETECT_INTERVAL_MS)
            {
                lastAudioDetectMs = millis();
                if (haySonido())
                {
                    audioDetectado = true;
                    Screen::showMessage(videoDetectado
                                            ? "Persona y audio detectada en el timbre. Analizando analisis inteligente..."
                                            : "Audio entrante detectada en el timbre");
                    BackendSession::activarAudio(true);
                }
            }

            static unsigned long lastVideoDetectMs = 0;
            if (!videoDetectado &&
                millis() - lastVideoDetectMs >= Config::Event::VIDEO_DETECT_INTERVAL_MS)
            {
                lastVideoDetectMs = millis();
                if (hayPersonaEnFrame())
                {
                    videoDetectado = true;
                    Screen::showMessage(audioDetectado
                                            ? "Persona y audio detectada en el timbre. Analizando analisis inteligente..."
                                            : "Persona detectada en el timbre");
                    BackendSession::activarVideo(true);
                }
            }
        }

        if (hangupPedido)
        {
            hangupPedido = false;

            endEvent();
        }
    }
}
