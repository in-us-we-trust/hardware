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
//   BACKEND: MQTT press (foto) + capture_image y, cuando hace falta, RTSP
//
// Flujo del BACKEND:
//   1. ANALIZANDO_FOTO  foto del timbre -> tiny model (persona si/no).
//   2. CONECTANDO       MQTT (mTLS) y press, SIEMPRE (haya o no persona). La
//                       foto va en el press solo si el tiny model vio una
//                       persona. Si el publish falla, se reintenta.
//   3. ACTIVO           llamada en curso. En paralelo:
//        - Deteccion local: sonido (mic) seguido; persona (tiny model) cada
//          EVAL_INTERVAL_MS mientras no se haya visto a nadie.
//        - Fotos al backend: cada CAPTURE_INTERVAL_MS se saca una foto, pasa
//          por el tiny model y SOLO si hay persona se manda (capture_image).
//        - Media (RTSP): no espera el ia_response de la IMAGE.
//            AUDIO: se manda apenas hay sonido.
//            VIDEO: se manda apenas hay persona.
//          El RTSP se abre con lo primero que haga falta y el otro track se
//          prende en vivo despues. Si la IMAGE vuelve STOP_DANGER, onRespuesta
//          corta el RTSP (y el gateway tambien corta su sesion).
//   FINALIZADO          STOP_DANGER: backend cerrado para este evento.
//
// Screen: linea de ESTADO (cada paso + mensajes "de progreso" rotativos para
// que el usuario vea que el analisis sigue) y RESULTADO fijo (la IA tiene
// prioridad sobre lo local; entre iguales gana el de mayor severidad).
// ---------------------------------------------------------------------------

namespace
{
    enum class Backend
    {
        OFF,             // sin evento
        ANALIZANDO_FOTO, // warm-up de la camara; despues foto + tiny model
        CONECTANDO,      // conectando al broker MQTT y mandando el press
        ACTIVO,          // press enviado: deteccion, capturas y media
        FINALIZADO       // STOP_DANGER: backend cerrado para este evento
    };

    // Estado del RTSP dentro de ACTIVO.
    enum class Media
    {
        CERRADA,   // sin RTSP
        ABRIENDO,  // negociando RTSPS
        PUBLICANDO // RTSP ready (stream_start enviado)
    };

    MediaSource *src = nullptr;

    bool eventoActivo = false;
    Backend backend = Backend::OFF;
    unsigned long estadoDesde = 0; // cuando se entro al estado actual
    String trackId = "";

    Media media = Media::CERRADA;
    unsigned long mediaDesde = 0;
    unsigned long ultimoFalloMediaMs = 0;
    bool falloMedia = false;
    // Que track se prendio en el RTSP (para prender el otro en vivo).
    bool audioEnviando = false;
    bool videoEnviando = false;

    // Resultado del tiny model sobre la foto del press.
    bool personaEnFoto = false;
    // Llego el ia_response de la IMAGE (lo marca onRespuesta).
    bool respuestaImagen = false;
    // Ya se reacciono a esa respuesta en el loop (aviso de monitoreo).
    bool respuestaProcesada = false;
    bool reintentandoPress = false;

    unsigned long ultimaEvaluacionMs = 0; // tiny model local
    unsigned long ultimoSonidoMs = 0;     // chequeo de mic
    unsigned long ultimaCapturaMs = 0;    // foto cada CAPTURE_INTERVAL_MS

    // Hay una foto (press o capture_image) esperando su ia_response IMAGE. No
    // se manda otra hasta que llegue (o pase CAPTURE_RESPONSE_TIMEOUT_MS): asi
    // las fotos no se acumulan en el gateway si el analisis tarda.
    bool fotoEnAnalisis = false;
    unsigned long fotoEnviadaMs = 0;

    // Pico del nivel de microfono desde la ultima vez que se evaluo. level() es
    // solo el RMS del ultimo chunk (20ms): si se lee espaciado, un sonido corto
    // entre chequeos se pierde. loop() va acumulando el maximo y haySonido()
    // lo consume.
    uint16_t picoAudio = 0;

    // Que ya se detecto en este evento. videoDetectado, una vez en true, queda.
    // audioDetectado se reevalua: se apaga tras AUDIO_SILENCE_TIMEOUT_MS sin
    // sonido (ultimoAudioDetectadoMs) y se vuelve a prender si hay sonido.
    bool audioDetectado = false;
    bool videoDetectado = false;
    unsigned long ultimoAudioDetectadoMs = 0;

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

    // --- Screen ---------------------------------------------------------------
    // Dos zonas:
    //   estado():    linea efimera (progreso, conexion, fallas). Se pisa sola.
    //   resultado(): analisis de la IA del backend o del tiny model / microfono
    //                local. Va al historial y a la tarjeta fija (Screen decide:
    //                gana el de mayor severidad del evento).
    // Los mensajes de progreso solo salen si la linea de estado estuvo quieta
    // un rato, asi no pisan enseguida un estado recien puesto.
    unsigned long ultimoMensajeMs = 0;
    uint8_t pasoProgreso = 0;

    void estado(const String &texto)
    {
        Screen::showStatus(texto);
        ultimoMensajeMs = millis();
    }

    // Resultado LOCAL (tiny model / microfono). Nunca pisa uno de la IA.
    void resultado(const String &texto, const String &severidad, const String &origen)
    {
        Screen::showResult(texto, severidad, origen, false);
    }

    const char *const ORIGEN_CAMARA = "Local - Camara";
    const char *const ORIGEN_MICROFONO = "Local - Microfono";

    void cambiarEstado(Backend nuevo)
    {
        backend = nuevo;
        estadoDesde = millis();
        pasoProgreso = 0;
    }

    // Muestra el siguiente mensaje de la lista si la pantalla estuvo quieta
    // al menos cadaMs. Da la sensacion de un analisis continuo.
    void progreso(const char *const *mensajes, size_t cantidad, uint32_t cadaMs)
    {
        if (cantidad == 0 || millis() - ultimoMensajeMs < cadaMs)
            return;
        estado(mensajes[pasoProgreso % cantidad]);
        pasoProgreso++;
    }

    const char *const PROGRESO_CON_PERSONA[] = {
        "Reconociendo a la persona en la imagen...",
        "Comparando el rostro con las personas registradas...",
        "Buscando objetos peligrosos en la imagen...",
    };
    const char *const PROGRESO_SIN_PERSONA[] = {
        "Buscando objetos peligrosos en la imagen...",
        "Analizando la escena frente a la puerta...",
        "Verificando si hay alguien en la imagen...",
    };
    const char *const PROGRESO_MONITOREO[] = {
        "Sin personas a la vista. Sigo monitoreando la entrada...",
        "Analizando la camara en busca de movimiento...",
        "Escuchando el entorno del timbre...",
    };
    const char *const PROGRESO_VIDEO[] = {
        "Analizando el comportamiento en video...",
        "Buscando objetos peligrosos en el video...",
        "Siguiendo el movimiento frente a la puerta...",
    };
    const char *const PROGRESO_AUDIO[] = {
        "Analizando la conversacion...",
        "Buscando senales de presion o amenaza en el audio...",
    };
    const char *const PROGRESO_AUDIO_VIDEO[] = {
        "Analizando el comportamiento en video...",
        "Analizando la conversacion...",
        "Buscando objetos peligrosos en el video...",
        "Buscando senales de presion o amenaza en el audio...",
    };

    template <size_t N>
    void progreso(const char *const (&mensajes)[N], uint32_t cadaMs)
    {
        progreso(mensajes, N, cadaMs);
    }

    void resetearEvento()
    {
        audioDetectado = false;
        videoDetectado = false;
        ultimoAudioDetectadoMs = 0;
        fotoSinAnalizar = false;
        fotoLen = 0;
        personaEnFoto = false;
        respuestaImagen = false;
        respuestaProcesada = false;
        reintentandoPress = false;
        fotoEnAnalisis = false;
        media = Media::CERRADA;
        falloMedia = false;
        audioEnviando = false;
        videoEnviando = false;
        picoAudio = 0;
    }

    void endEvent()
    {
        Serial.println("[FLOW] hangup local -> cierro el evento");
        estado("Finalizando llamada...");
        // cerrarSesion frena el RTSP (si estaba) y publica stream_end (si hubo
        // press). Se llama siempre: el gateway necesita el stream_end para
        // dejar de reintentar el RTSP aunque nunca se haya abierto.
        if (backend != Backend::OFF)
            BackendSession::cerrarSesion(trackId);

        cambiarEstado(Backend::OFF);
        resetearEvento();

        stopMjpegTask();
        Microphone.end();
        Screen::clear();
        eventoActivo = false;

        PowerMode::low(); // camara off + 80MHz + WiFi sleep
    }

    // VIDEO: corre el tiny model sobre la foto del timbre si todavia no se
    // analizo; si no, sobre un frame nuevo.
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

    String etiquetaChunk(const String &chunkType)
    {
        if (chunkType == "IMAGE")
            return "Imagen";
        if (chunkType == "VIDEO")
            return "Video";
        if (chunkType == "AUDIO")
            return "Audio";
        return chunkType;
    }

    void mostrarEnScreen(const MqttClient::IaResponse &r)
    {
        String texto = r.message.length() > 0 ? r.message : String("Analisis completado");
        String sev = r.severity;
        if (r.isStopDanger())
        {
            texto = "PELIGRO: " + texto;
            sev = "PELIGROSO"; // un STOP_DANGER siempre queda fijo arriba
        }
        Screen::showResult(texto, sev, "IA - " + etiquetaChunk(r.chunkType), true);
    }

    void onRespuesta(const MqttClient::IaResponse &r)
    {
        mostrarEnScreen(r);

        if (r.chunkType == "IMAGE")
        {
            respuestaImagen = true;
            fotoEnAnalisis = false; // se puede mandar la proxima captura
        }

        if (r.isStopDanger())
        {
            Serial.println("[FLOW] STOP_DANGER -> cierro SOLO el backend (el stream local sigue)");
            BackendSession::cerrarSesion(trackId); // frena RTSP + stream_end
            media = Media::CERRADA;
            audioEnviando = false;
            videoEnviando = false;
            cambiarEstado(Backend::FINALIZADO);
        }
        else
        {
            Serial.println("[FLOW] CONTINUE -> backend sigue");
        }
    }
}

namespace
{
    // Publica el press. La foto va SOLO si el tiny model vio una persona en
    // ella; si no, el press sale sin imagen (es opcional). Mensaje en pantalla
    // segun el caso, para que el usuario sepa que se esta haciendo.
    bool enviarPress()
    {
        String fotoB64 = personaEnFoto ? CameraSource::toBase64(foto, fotoLen) : String("");
        if (!BackendSession::enviarFotoBase64(trackId, fotoB64))
        {
            Serial.println("[FLOW] fallo el press");
            return false;
        }
        Serial.printf("[FLOW] press enviado trackId=%s (persona local=%s, %s)\n",
                      trackId.c_str(), personaEnFoto ? "si" : "no",
                      personaEnFoto ? "con foto" : "sin foto");
        estado(personaEnFoto ? "Reconociendo a la persona en la imagen..."
                             : "Sin personas a la vista. Sigo monitoreando la entrada...");
        return true;
    }

    // Marca persona detectada (una sola vez) y avisa en pantalla.
    void marcarPersona()
    {
        if (videoDetectado)
            return;
        videoDetectado = true;
        resultado("Persona detectada en la puerta", "NORMAL", ORIGEN_CAMARA);
    }

    // --- Deteccion local -------------------------------------------------------
    // Sonido: chequeo barato y seguido (AUDIO_DETECT_INTERVAL_MS), siempre: si
    // no hay sonido durante AUDIO_SILENCE_TIMEOUT_MS se apaga el audio.
    // Persona: tiny model cada EVAL_INTERVAL_MS, solo mientras no se vio a nadie
    // (despues lo sigue corriendo la captura cada 5 s).
    void detectarLocal()
    {
        if (millis() - ultimoSonidoMs >= Config::Event::AUDIO_DETECT_INTERVAL_MS)
        {
            ultimoSonidoMs = millis();
            if (haySonido())
            {
                ultimoAudioDetectadoMs = millis();
                if (!audioDetectado)
                {
                    audioDetectado = true;
                    resultado("Se escucha a alguien en la puerta", "NORMAL", ORIGEN_MICROFONO);
                }
            }
            else if (audioDetectado &&
                     millis() - ultimoAudioDetectadoMs >= Config::Event::AUDIO_SILENCE_TIMEOUT_MS)
            {
                audioDetectado = false;
                Serial.printf("[FLOW] %lu ms sin sonido -> audio OFF\n",
                              (unsigned long)Config::Event::AUDIO_SILENCE_TIMEOUT_MS);
            }
        }

        if (!videoDetectado &&
            millis() - ultimaEvaluacionMs >= Config::Event::EVAL_INTERVAL_MS)
        {
            ultimaEvaluacionMs = millis();
            if (hayPersonaEnFrame())
                marcarPersona();
        }
    }

    // --- Fotos al backend ------------------------------------------------------
    // Cada CAPTURE_INTERVAL_MS: foto nueva -> tiny model -> se manda como
    // capture_image SOLO si hay persona.
    // Una sola foto en analisis a la vez: si la anterior (press o captura)
    // todavia no tuvo su ia_response IMAGE, se espera. Si no llega en
    // CAPTURE_RESPONSE_TIMEOUT_MS se sigue igual (para no cortar las capturas).
    void capturaPeriodica()
    {
        if (millis() - ultimaCapturaMs < Config::Event::CAPTURE_INTERVAL_MS)
            return;
        if (fotoEnAnalisis)
        {
            if (millis() - fotoEnviadaMs < Config::Event::CAPTURE_RESPONSE_TIMEOUT_MS)
                return; // todavia se esta analizando la anterior
            Serial.println("[FLOW] sin ia_response de la foto anterior: sigo con las capturas");
            fotoEnAnalisis = false;
        }
        ultimaCapturaMs = millis();
        ultimaEvaluacionMs = millis(); // ya corre el tiny model aca

        if (!hayPersonaEnFrame()) // captura un frame nuevo
        {
            Serial.println("[FLOW] captura sin persona: no se manda");
            return;
        }
        marcarPersona();
        if (BackendSession::enviarCaptura(trackId, foto, fotoLen))
        {
            fotoEnAnalisis = true;
            fotoEnviadaMs = millis();
            Serial.printf("[FLOW] capture_image enviada (%u bytes)\n", (unsigned)fotoLen);
        }
        else
            Serial.println("[FLOW] no se pudo enviar la capture_image");
    }

    // --- Media (RTSP) ----------------------------------------------------------
    // No se espera el ia_response de la IMAGE (el analisis puede tardar ~1 min):
    // AUDIO apenas hay sonido, VIDEO apenas hay persona. Un STOP_DANGER de la
    // IMAGE corta el RTSP en onRespuesta.
    bool quiereAudio() { return audioDetectado; }
    bool quiereVideo() { return videoDetectado; }

    const char *textoTransmitiendo(bool video, bool audio)
    {
        if (video && audio)
            return "Transmitiendo video y audio. Analisis en vivo en curso...";
        if (video)
            return "Transmitiendo video. Analisis en vivo en curso...";
        return "Transmitiendo audio. Analisis en vivo en curso...";
    }

    void gestionarMedia()
    {
        bool audio = quiereAudio();
        bool video = quiereVideo();

        switch (media)
        {
        case Media::CERRADA:
            if (!audio && !video)
                return;
            // Tras un fallo, se espera MEDIA_RETRY_MS antes de reintentar.
            if (falloMedia && millis() - ultimoFalloMediaMs < Config::Event::MEDIA_RETRY_MS)
                return;
            estado(video && audio ? "Iniciando video y audio para analisis en vivo..."
                   : video        ? "Iniciando video para analisis en vivo..."
                                  : "Iniciando audio para analisis en vivo...");
            if (BackendSession::iniciarMedia(trackId))
            {
                media = Media::ABRIENDO;
                mediaDesde = millis();
            }
            else
            {
                Serial.println("[FLOW] RTSP no arranco: reintento mas tarde");
                falloMedia = true;
                ultimoFalloMediaMs = millis();
            }
            return;

        case Media::ABRIENDO:
            if (BackendSession::mediaLista())
            {
                BackendSession::activarAudio(audio);
                BackendSession::activarVideo(video);
                audioEnviando = audio;
                videoEnviando = video;
                BackendSession::avisarInicioMedia(trackId); // stream_start
                Serial.println("[FLOW] RTSP ready -> stream_start");
                media = Media::PUBLICANDO;
                falloMedia = false;
                estado(textoTransmitiendo(video, audio));
            }
            else if (BackendSession::mediaFallo() ||
                     millis() - mediaDesde > Config::Event::RTSP_READY_TIMEOUT_MS)
            {
                Serial.println("[FLOW] RTSP no quedo ready: reintento mas tarde");
                BackendSession::detenerMedia();
                media = Media::CERRADA;
                falloMedia = true;
                ultimoFalloMediaMs = millis();
                estado("No se pudo iniciar la transmision. Reintentando...");
            }
            return;

        case Media::PUBLICANDO:
            // Prende en vivo el track que falte (sin reconectar). El audio
            // tambien se apaga si quedo en silencio.
            if (audio && !audioEnviando)
            {
                BackendSession::activarAudio(true);
                audioEnviando = true;
                estado(textoTransmitiendo(videoEnviando, true));
            }
            else if (!audio && audioEnviando)
            {
                BackendSession::activarAudio(false);
                audioEnviando = false;
                if (videoEnviando)
                    estado(textoTransmitiendo(true, false));
            }
            if (video && !videoEnviando)
            {
                BackendSession::activarVideo(true);
                videoEnviando = true;
                estado(textoTransmitiendo(true, audioEnviando));
            }
            return;
        }
    }

    // --- Pantalla: progreso segun lo que se este haciendo ------------------------
    void mostrarProgreso()
    {
        if (media == Media::PUBLICANDO)
        {
            if (videoEnviando && audioEnviando)
                progreso(PROGRESO_AUDIO_VIDEO, Config::Event::PROGRESO_SESION_MS);
            else if (videoEnviando)
                progreso(PROGRESO_VIDEO, Config::Event::PROGRESO_SESION_MS);
            else
                progreso(PROGRESO_AUDIO, Config::Event::PROGRESO_SESION_MS);
            return;
        }
        if (media == Media::ABRIENDO)
            return;
        // Solo se "espera a la IA" si hay una imagen mandada o por mandar: la
        // del press (personaEnFoto) o una captura con persona (videoDetectado).
        if (!respuestaImagen && (personaEnFoto || videoDetectado))
        {
            if (videoDetectado)
                progreso(PROGRESO_CON_PERSONA, Config::Event::PROGRESO_ESPERA_MS);
            else
                progreso(PROGRESO_SIN_PERSONA, Config::Event::PROGRESO_ESPERA_MS);
            return;
        }
        progreso(PROGRESO_MONITOREO, Config::Event::PROGRESO_SESION_MS);
    }

    void loopActivo()
    {
        detectarLocal();
        capturaPeriodica();

        // Primera vez que se ve la respuesta: si no hay nada para mandar,
        // avisar que se sigue monitoreando.
        if (respuestaImagen && !respuestaProcesada)
        {
            respuestaProcesada = true;
            if (media == Media::CERRADA && !quiereAudio() && !quiereVideo())
                estado("Sin personas a la vista. Sigo monitoreando la entrada...");
        }

        gestionarMedia();
        mostrarProgreso();
    }
}

namespace EventFlow
{
    void initialize(const String &deviceId, MediaSource *source)
    {
        src = source;
        if (src)
            src->begin();
        // BackendSession es dueño del transporte (MQTT + RTSP). Le pasamos las
        // fuentes de media que el stream RTSP va a publicar.
        BackendSession::initialize(deviceId, &Camera, &Microphone);

        // pasa los callbacks
        BackendSession::onRespuesta(onRespuesta);
        Screen::onHangup(EventFlow::terminar);

        eventoActivo = false;
        cambiarEstado(Backend::OFF);
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

        // Alimenta el visor que ve el usuario. Es un task que funciona en el 2do core
        startMjpegTask();
        hangupPedido = false;
        eventoActivo = true;

        resetearEvento();

        estado("Tocaron el timbre. Analizando la imagen...");
        // La foto se saca despues de FOTO_WARMUP_MS (la camara recien prendida
        // da frames oscuros).
        cambiarEstado(Backend::ANALIZANDO_FOTO);
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

        switch (backend)
        {
        case Backend::OFF:
        case Backend::FINALIZADO:
            break;

        case Backend::ANALIZANDO_FOTO:
            if (millis() - estadoDesde >= Config::Event::FOTO_WARMUP_MS)
            {
                // 1) foto + tiny model
                fotoSinAnalizar = capturarFoto();
                personaEnFoto = hayPersonaEnFrame();
                videoDetectado = personaEnFoto;
                audioDetectado = haySonido();
                if (audioDetectado)
                    ultimoAudioDetectadoMs = millis();
                // Resultado LOCAL (tiny model / microfono): va a la tarjeta
                // hasta que llegue algo de la IA.
                resultado(personaEnFoto ? "Persona detectada en la puerta"
                                        : "No se ve a nadie en la imagen",
                          "NORMAL", ORIGEN_CAMARA);
                if (audioDetectado)
                    resultado("Se escucha a alguien en la puerta", "NORMAL", ORIGEN_MICROFONO);
                estado(personaEnFoto ? "Enviando la imagen para analizarla..."
                                     : "Avisando al servicio de analisis...");
                // 2) conectar para mandar el press (siempre, haya o no persona)
                BackendSession::conectar();
                cambiarEstado(Backend::CONECTANDO);
            }
            break;

        case Backend::CONECTANDO:
            if (BackendSession::estaListo())
            {
                // Tras un press fallido, espera un poco antes de reintentar.
                if (reintentandoPress && millis() - estadoDesde < Config::Event::PRESS_RETRY_MS)
                    break;
                if (enviarPress())
                {
                    reintentandoPress = false;
                    cambiarEstado(Backend::ACTIVO);
                    ultimaEvaluacionMs = millis();
                    ultimoSonidoMs = millis();
                    ultimaCapturaMs = millis();
                    // La foto del press tambien cuenta: la primera captura
                    // espera su respuesta. Si el press salio sin foto no
                    // hay nada que esperar.
                    fotoEnAnalisis = personaEnFoto;
                    fotoEnviadaMs = millis();
                }
                else
                {
                    // Sin press no hay sesion: se reintenta.
                    estado(personaEnFoto ? "No se pudo enviar la imagen. Reintentando..."
                                         : "No se pudo avisar al servicio de analisis. Reintentando...");
                    reintentandoPress = true;
                    BackendSession::conectar();
                    cambiarEstado(Backend::CONECTANDO);
                }
            }
            else if (millis() - estadoDesde > Config::Event::BACKEND_CONNECT_TIMEOUT_MS)
            {
                Serial.println("[FLOW] timeout conectando backend: reintento");
                estado("Sin conexion con el servicio de analisis. Reintentando...");
                BackendSession::conectar();
                cambiarEstado(Backend::CONECTANDO);
            }
            break;

        case Backend::ACTIVO:
            loopActivo();
            break;
        }

        if (hangupPedido)
        {
            hangupPedido = false;

            endEvent();
        }
    }
}
