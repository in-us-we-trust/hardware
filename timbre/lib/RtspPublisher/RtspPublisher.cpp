#include "RtspPublisher.h"
#include "Config.h"
#include "Certs.h" // CA_CERT: valida el server cert de MediaMTX
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>
#include "mbedtls/base64.h"

// Timeout del handshake TLS, en SEGUNDOS (WiFiClientSecure::setHandshakeTimeout).
#ifndef RTSP_TLS_HANDSHAKE_TIMEOUT_S
#define RTSP_TLS_HANDSHAKE_TIMEOUT_S 10
#endif

#define RTP_PT_JPEG 26 // video JPEG (RFC 2435)
#define RTP_PT_PCMU 0  // audio G.711 u-law

#define RTP_CHAN_VIDEO 0 // interleaved channel para RTP video
#define RTP_CHAN_AUDIO 2 // interleaved channel para RTP audio

#define MAX_RTP_PACKET 1400

namespace
{
    using State = RtspPublisher::State;

    // RTSPS: TLS solo del lado del server (MediaMTX no pide client cert). La
    // identidad va por user/pass (Basic) en cada request.
    WiFiClientSecure tcp;
    MediaSource *videoSrc = nullptr;
    AudioSource *audioSrc = nullptr;
    // "has*" = track NEGOCIADO (declarado en el SDP; su fuente existe). Fijo
    // durante la sesion. "*Activo" = si ese track esta ENVIANDO RTP ahora
    // mismo; se prende/apaga en vivo con setVideoActivo()/setAudioActivo().
    bool hasVideo = false;
    bool hasAudio = false;
    bool videoActivo = false;
    bool audioActivo = false;

    String mtxHost;
    String mtxPath;    // {deviceId}/doorbell/{trackId}; se fija en start()
    String authHeader; // "Authorization: Basic ...\r\n" (precalculado)

    State st = State::Idle;
    uint32_t cseq = 1;
    String sessionId;

    // Video RTP state
    uint16_t vSeq = 0;
    uint32_t vSsrc = 0;
    uint32_t vTimestamp = 0;
    uint32_t frameCount = 0;
    unsigned long lastFrameMs = 0;

    // Audio RTP state
    uint16_t aSeq = 0;
    uint32_t aSsrc = 0;
    uint32_t aTimestamp = 0;

    uint8_t pkt[MAX_RTP_PACKET + 8];

    // Copia PROPIA del frame que se esta mandando (PSRAM). Antes se mandaba
    // directo del buffer interno de CameraSource y el task MJPEG (otro core)
    // lo pisaba a mitad del envio -> JPEG con el final roto en el gateway.
    uint8_t *frameBuf = nullptr;
    size_t frameCap = 0;

    // Timestamp RTP de video = tiempo real desde que arranco el stream (90 kHz),
    // no +1/fps fijo: si el loop va lento, el video no se "acelera".
    unsigned long streamStartMs = 0;

    // Sin credenciales en la URL: van en el header Authorization.
    String rtspUrl()
    {
        return "rtsps://" + mtxHost + ":" + String(Config::Media::PORT) + "/" + mtxPath;
    }

    String basicAuthHeader(const String &user, const String &pass)
    {
        String plain = user + ":" + pass;
        size_t need = 0;
        mbedtls_base64_encode(nullptr, 0, &need, (const uint8_t *)plain.c_str(), plain.length());
        String out;
        uint8_t *buf = (uint8_t *)malloc(need);
        size_t written = 0;
        if (buf && mbedtls_base64_encode(buf, need, &written,
                                         (const uint8_t *)plain.c_str(), plain.length()) == 0)
            out = "Authorization: Basic " + String((const char *)buf) + "\r\n";
        free(buf);
        return out;
    }

    // --- RTSP helpers -----------------------------------------------------
    // Todos los requests llevan Authorization (Basic preventivo): MediaMTX usa
    // rtspAuthMethods [basic] por defecto y valida en cada request.
    void sendRequest(const String &method, const String &url, const String &extraHeaders,
                     const String &body = "")
    {
        String req = method + " " + url + " RTSP/1.0\r\n";
        req += "CSeq: " + String(cseq++) + "\r\n";
        req += "User-Agent: timbre-esp32\r\n";
        req += authHeader;
        if (sessionId.length() > 0)
            req += "Session: " + sessionId + "\r\n";
        req += extraHeaders;
        if (body.length() > 0)
            req += "Content-Length: " + String(body.length()) + "\r\n";
        req += "\r\n";
        req += body;
        tcp.print(req);
        Serial.printf("[RTSP] -> %s (CSeq %u)\n", method.c_str(), cseq - 1);
    }

    String readResponse(uint32_t timeoutMs = 4000)
    {
        String resp;
        unsigned long start = millis();
        while (millis() - start < timeoutMs)
        {
            while (tcp.available())
            {
                char c = tcp.read();
                resp += c;
                if (resp.endsWith("\r\n\r\n"))
                    return resp;
            }
            delay(1);
        }
        return resp;
    }

    bool responseOk(const String &resp) { return resp.startsWith("RTSP/1.0 200"); }

    // Loguea la primera linea de una respuesta no-200 (401 = credenciales, 400
    // = path/SDP, etc.) para no tener que adivinar.
    void logBadResponse(const char *what, const String &resp)
    {
        int nl = resp.indexOf("\r\n");
        String status = nl > 0 ? resp.substring(0, nl) : resp;
        Serial.printf("[RTSP] %s respondio: %s\n", what, status.c_str());
        if (resp.startsWith("RTSP/1.0 401"))
            Serial.println("[RTSP] 401: user/pass rechazados o sin permiso de publish en ese path "
                           "(ver authInternalUsers en mediamtx.secure.yml)");
    }

    String extractSession(const String &resp)
    {
        int idx = resp.indexOf("Session:");
        if (idx < 0)
            return "";
        int nl = resp.indexOf("\r\n", idx);
        String s = resp.substring(idx + 8, nl);
        s.trim();
        int semi = s.indexOf(';');
        if (semi >= 0)
            s = s.substring(0, semi);
        s.trim();
        return s;
    }

    String buildSdp()
    {
        String sdp;
        sdp += "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 127.0.0.1\r\n";
        sdp += "s=timbre\r\n";
        sdp += "c=IN IP4 0.0.0.0\r\n";
        sdp += "t=0 0\r\n";
        // Track 0: video JPEG (solo si el video esta activo en este evento)
        if (hasVideo)
        {
            sdp += "m=video 0 RTP/AVP 26\r\n";
            sdp += "a=rtpmap:26 JPEG/90000\r\n";
            sdp += "a=control:streamid=0\r\n";
        }
        // Track 1: audio G.711 u-law (solo si el audio esta activo)
        if (hasAudio)
        {
            sdp += "m=audio 0 RTP/AVP 0\r\n";
            sdp += "a=rtpmap:0 PCMU/8000\r\n";
            sdp += "a=control:streamid=1\r\n";
        }
        return sdp;
    }

    // SETUP de cada track. Incluyen Session solo si ya lo tenemos (el primer
    // SETUP de la sesion todavia no lo tiene; el segundo si).
    // RTP/AVP/TCP (no SAVP): con RTSPS + TCP interleaved el RTP ya viaja
    // dentro del TLS; MediaMTX solo exige SAVP para UDP.
    void sendSetupVideo()
    {
        sendRequest("SETUP", rtspUrl() + "/streamid=0",
                    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n");
    }

    void sendSetupAudio()
    {
        sendRequest("SETUP", rtspUrl() + "/streamid=1",
                    "Transport: RTP/AVP/TCP;unicast;interleaved=2-3;mode=record\r\n");
    }

    void fail(const char *why)
    {
        Serial.printf("[RTSP] error: %s\n", why);
        st = State::Error;
        tcp.stop();
    }

    // --- JPEG parser (sin cambios) ----------------------------------------
    struct JpegInfo
    {
        uint16_t width = 0, height = 0;
        uint8_t type = 0;
        const uint8_t *qt[2] = {nullptr, nullptr};
        bool haveQt[2] = {false, false};
        size_t scanStart = 0, scanLen = 0;
        bool hasDri = false;
        bool ok = false;
    };

    JpegInfo parseJpeg(const uint8_t *d, size_t len)
    {
        JpegInfo j;
        if (len < 4 || d[0] != 0xFF || d[1] != 0xD8)
            return j;
        size_t p = 2;
        while (p + 4 <= len)
        {
            if (d[p] != 0xFF)
            {
                p++;
                continue;
            }
            uint8_t marker = d[p + 1];
            if (marker == 0xD9)
                break;
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            {
                p += 2;
                continue;
            }
            uint16_t seglen = (d[p + 2] << 8) | d[p + 3];
            size_t segStart = p + 4;
            size_t segEnd = p + 2 + seglen;
            if (segEnd > len)
                break;

            if (marker == 0xDB)
            {
                size_t q = segStart;
                while (q < segEnd)
                {
                    uint8_t pqtq = d[q++];
                    uint8_t pq = pqtq >> 4;
                    uint8_t tq = pqtq & 0x0F;
                    if (pq == 0 && tq < 2 && q + 64 <= segEnd)
                    {
                        j.qt[tq] = &d[q];
                        j.haveQt[tq] = true;
                        q += 64;
                    }
                    else
                        break;
                }
            }
            else if (marker == 0xC0)
            {
                j.height = (d[segStart + 1] << 8) | d[segStart + 2];
                j.width = (d[segStart + 3] << 8) | d[segStart + 4];
                uint8_t nc = d[segStart + 5];
                if (nc >= 1)
                {
                    uint8_t v = d[segStart + 7] & 0x0F;
                    j.type = (v == 2) ? 1 : 0;
                }
            }
            else if (marker == 0xC2)
            {
                return j;
            }
            else if (marker == 0xDD)
            {
                j.hasDri = true;
            }
            else if (marker == 0xDA)
            {
                j.scanStart = segEnd;
                size_t end = len;
                if (len >= 2 && d[len - 2] == 0xFF && d[len - 1] == 0xD9)
                    end = len - 2;
                j.scanLen = end - j.scanStart;
                j.ok = j.haveQt[0] && j.haveQt[1] && !j.hasDri;
                return j;
            }
            p = segEnd;
        }
        return j;
    }

    // --- RTP send ---------------------------------------------------------
    bool sendInterleaved(uint8_t channel, uint8_t *buf, uint16_t rtpLen)
    {
        buf[0] = 0x24; // '$'
        buf[1] = channel;
        buf[2] = rtpLen >> 8;
        buf[3] = rtpLen & 0xFF;
        size_t total = 4 + rtpLen;
        return tcp.write(buf, total) == total;
    }

    void sendVideoFrame(const uint8_t *jpeg, size_t len)
    {
        JpegInfo j = parseJpeg(jpeg, len);
        if (!j.ok)
            return;
        if ((j.width / 8) > 255 || (j.height / 8) > 255)
            return;

        vTimestamp = (uint32_t)((millis() - streamStartMs) * 90UL);

        const uint8_t *scan = jpeg + j.scanStart;
        size_t remaining = j.scanLen;
        size_t offset = 0;

        while (remaining > 0)
        {
            bool first = (offset == 0);
            size_t hdrSize = 12 + 8 + (first ? (4 + 128) : 0);
            size_t maxPayload = MAX_RTP_PACKET - hdrSize;
            size_t chunk = remaining < maxPayload ? remaining : maxPayload;
            bool last = (chunk == remaining);

            uint8_t *w = pkt + 4;

            w[0] = 0x80;
            w[1] = (last ? 0x80 : 0x00) | RTP_PT_JPEG;
            w[2] = vSeq >> 8;
            w[3] = vSeq & 0xFF;
            vSeq++;
            w[4] = vTimestamp >> 24;
            w[5] = vTimestamp >> 16;
            w[6] = vTimestamp >> 8;
            w[7] = vTimestamp & 0xFF;
            w[8] = vSsrc >> 24;
            w[9] = vSsrc >> 16;
            w[10] = vSsrc >> 8;
            w[11] = vSsrc & 0xFF;
            w += 12;

            w[0] = 0;
            w[1] = (offset >> 16) & 0xFF;
            w[2] = (offset >> 8) & 0xFF;
            w[3] = offset & 0xFF;
            w[4] = j.type;
            w[5] = 255;
            w[6] = j.width / 8;
            w[7] = j.height / 8;
            w += 8;

            if (first)
            {
                w[0] = 0;
                w[1] = 0;
                w[2] = 0;
                w[3] = 128;
                w += 4;
                memcpy(w, j.qt[0], 64);
                w += 64;
                memcpy(w, j.qt[1], 64);
                w += 64;
            }

            memcpy(w, scan + offset, chunk);
            w += chunk;

            uint16_t rtpLen = (w - (pkt + 4));
            if (!sendInterleaved(RTP_CHAN_VIDEO, pkt, rtpLen))
            {
                fail("write RTP video fallo");
                return;
            }

            offset += chunk;
            remaining -= chunk;
        }

        frameCount++;
        if (frameCount % 30 == 0)
            Serial.printf("[RTSP] video: %u frames\n", frameCount);
    }

    void sendAudioChunk(const uint8_t *ulaw, size_t len)
    {
        // RTP header (12 bytes) + payload (160 bytes u-law)
        uint8_t *w = pkt + 4;

        w[0] = 0x80;
        w[1] = 0x80 | RTP_PT_PCMU; // marker=1 (convencion para audio)
        w[2] = aSeq >> 8;
        w[3] = aSeq & 0xFF;
        aSeq++;
        w[4] = aTimestamp >> 24;
        w[5] = aTimestamp >> 16;
        w[6] = aTimestamp >> 8;
        w[7] = aTimestamp & 0xFF;
        w[8] = aSsrc >> 24;
        w[9] = aSsrc >> 16;
        w[10] = aSsrc >> 8;
        w[11] = aSsrc & 0xFF;
        w += 12;

        memcpy(w, ulaw, len);
        w += len;

        aTimestamp += len; // cada byte u-law = 1 sample a 8kHz

        uint16_t rtpLen = (w - (pkt + 4));
        if (!sendInterleaved(RTP_CHAN_AUDIO, pkt, rtpLen))
        {
            fail("write RTP audio fallo");
            return;
        }

        // Log cada ~50 chunks (~1s de audio) para confirmar que sale.
        static uint32_t audioCount = 0;
        audioCount++;
        if (audioCount % 50 == 0)
            Serial.printf("[RTSP] audio: %u chunks enviados\n", audioCount);
    }
}

namespace RtspPublisher
{
    void initialize(const String &mediaMtxHost, const String &user, const String &pass,
                    MediaSource *source, AudioSource *audio)
    {
        mtxHost = mediaMtxHost;
        mtxPath = "";
        authHeader = basicAuthHeader(user, pass);
        videoSrc = source;
        audioSrc = audio;
        // hasVideo/hasAudio se deciden por evento en start(); aca solo guardamos
        // que fuentes estan disponibles.
        st = State::Idle;
        vSsrc = esp_random();
        aSsrc = esp_random();
        tcp.setHandshakeTimeout(RTSP_TLS_HANDSHAKE_TIMEOUT_S);

        if (!frameBuf)
        {
            frameCap = Config::Media::FRAME_BUF_SIZE;
            frameBuf = (uint8_t *)(psramFound() ? ps_malloc(frameCap) : malloc(frameCap));
            if (!frameBuf)
            {
                frameCap = 0;
                Serial.println("[RTSP] sin memoria para el buffer de frame: video deshabilitado");
            }
        }
        Serial.printf("[RTSP] configurado -> rtsps://%s:%d (user %s, tls-name %s, video %s, audio %s)\n",
                      mtxHost.c_str(), Config::Media::PORT, user.c_str(),
                      Config::Media::TLS_HOSTNAME,
                      videoSrc ? "si" : "no", audioSrc ? "si" : "no");
    }

    bool start(const String &path)
    {
        mtxPath = path;
        // Negociamos todos los tracks con fuente disponible; el envio arranca
        // apagado y lo prende EventFlow cuando decide (deteccion).
        hasVideo = (videoSrc != nullptr && frameBuf != nullptr);
        hasAudio = (audioSrc != nullptr);
        videoActivo = false;
        audioActivo = false;

        if (!hasVideo && !hasAudio)
        {
            fail("sin fuentes para publicar (ni video ni audio)");
            return false;
        }

        Serial.printf("[RTSP] conectando a %s (negociando video %s, audio %s)...\n",
                      rtspUrl().c_str(), hasVideo ? "si" : "no", hasAudio ? "si" : "no");

        IPAddress ip;
        if (!WiFi.hostByName(mtxHost.c_str(), ip))
        {
            fail("no se pudo resolver el host de MediaMTX");
            return false;
        }
        // Handshake TLS (bloqueante, ~1-3 s). Se conecta a la IP pero valida el
        // server cert contra TLS_HOSTNAME (mismo problema de SAN IP que MQTT).
        tcp.stop();
        if (!tcp.connect(ip, Config::Media::PORT, Config::Media::TLS_HOSTNAME,
                         CA_CERT, nullptr, nullptr))
        {
            char err[128] = {0};
            int code = tcp.lastError(err, sizeof(err));
            Serial.printf("[RTSP] handshake TLS fallo (%d): %s\n", code, err);
            fail("no se pudo abrir RTSPS a MediaMTX");
            return false;
        }

        cseq = 1;
        sessionId = "";
        frameCount = 0;
        st = State::Announcing;
        sendRequest("ANNOUNCE", rtspUrl(), "Content-Type: application/sdp\r\n", buildSdp());
        return true;
    }

    void stop()
    {
        if (st == State::Idle)
            return;
        if (tcp.connected())
            sendRequest("TEARDOWN", rtspUrl(), "");
        tcp.stop();
        st = State::Idle;
        Serial.println("[RTSP] detenido (TEARDOWN)");
    }

    bool isPublishing() { return st == State::Publishing; }
    String streamUrl() { return rtspUrl(); }
    State state() { return st; }

    void setVideoActivo(bool activo)
    {
        if (!hasVideo)
        {
            if (activo)
                Serial.println("[RTSP] no se puede activar video: track no negociado");
            return;
        }
        if (videoActivo != activo)
        {
            videoActivo = activo;
            Serial.printf("[RTSP] video %s\n", activo ? "ON" : "OFF");
        }
    }

    void setAudioActivo(bool activo)
    {
        if (!hasAudio)
        {
            if (activo)
                Serial.println("[RTSP] no se puede activar audio: track no negociado");
            return;
        }
        if (audioActivo != activo)
        {
            audioActivo = activo;
            Serial.printf("[RTSP] audio %s\n", activo ? "ON" : "OFF");
        }
    }

    void loop()
    {
        switch (st)
        {
        case State::Announcing:
        {
            if (!tcp.available())
                return;
            String resp = readResponse();
            if (!responseOk(resp))
            {
                logBadResponse("ANNOUNCE", resp);
                fail("ANNOUNCE no-200");
                return;
            }

            // SETUP del primer track activo: video si esta, si no audio.
            if (hasVideo)
            {
                st = State::SettingUpVideo;
                sendSetupVideo();
            }
            else
            {
                st = State::SettingUpAudio;
                sendSetupAudio();
            }
            break;
        }
        case State::SettingUpVideo:
        {
            if (!tcp.available())
                return;
            String resp = readResponse();
            if (!responseOk(resp))
            {
                logBadResponse("SETUP video", resp);
                fail("SETUP video no-200");
                return;
            }

            String sid = extractSession(resp);
            if (sid.length() > 0)
                sessionId = sid;

            if (hasAudio)
            {
                st = State::SettingUpAudio;
                sendSetupAudio();
            }
            else
            {
                // Solo video: directo a RECORD
                st = State::Recording;
                sendRequest("RECORD", rtspUrl(), "Range: npt=0.000-\r\n");
            }
            break;
        }
        case State::SettingUpAudio:
        {
            if (!tcp.available())
                return;
            String resp = readResponse();
            if (!responseOk(resp))
            {
                logBadResponse("SETUP audio", resp);
                fail("SETUP audio no-200");
                return;
            }

            // Si el audio fue el PRIMER SETUP (modo solo-audio), el Session sale
            // de aca. Si vino despues del video, ya lo teniamos (idempotente).
            String sid = extractSession(resp);
            if (sid.length() > 0)
                sessionId = sid;

            st = State::Recording;
            sendRequest("RECORD", rtspUrl(), "Range: npt=0.000-\r\n");
            break;
        }
        case State::Recording:
        {
            if (!tcp.available())
                return;
            String resp = readResponse();
            if (!responseOk(resp))
            {
                logBadResponse("RECORD", resp);
                fail("RECORD no-200");
                return;
            }
            st = State::Publishing;
            lastFrameMs = 0;
            streamStartMs = millis();
            Serial.println("[RTSP] LISTO para publicar (negociado: " +
                           String(hasVideo ? "video" : "") +
                           String(hasVideo && hasAudio ? " + " : "") +
                           String(hasAudio ? "audio" : "") +
                           "). Envio arranca apagado; lo activa EventFlow.");
            break;
        }
        case State::Publishing:
        {
            if (!tcp.connected())
            {
                fail("TCP caido durante Publishing");
                return;
            }

            // MediaMTX manda RTCP (receiver reports) por el mismo socket. No lo
            // usamos, pero hay que leerlo para que no se llene el buffer.
            {
                uint8_t sink[128];
                while (tcp.available())
                    tcp.read(sink, sizeof(sink));
            }

            // Video: a la cadencia del source. Solo si el track fue negociado
            // Y esta activado en vivo (EventFlow decide).
            if (hasVideo && videoActivo)
            {
                int fps = videoSrc->videoFps() > 0 ? videoSrc->videoFps() : 10;
                unsigned long interval = 1000 / fps;
                if (millis() - lastFrameMs >= interval)
                {
                    lastFrameMs = millis();
                    size_t flen = 0;
                    if (videoSrc->copyVideoFrame(frameBuf, frameCap, &flen))
                        sendVideoFrame(frameBuf, flen);
                }
            }

            // Audio: manda TODOS los chunks encolados (AudioSource acumula lo que
            // se junto mientras el loop estaba ocupado). Idem: negociado Y
            // activado en vivo. Tope por vuelta para no monopolizar el loop.
            if (hasAudio && audioActivo && audioSrc)
            {
                const uint8_t *achunk;
                size_t alen;
                int sent = 0;
                while (st == State::Publishing && sent < 64 &&
                       audioSrc->nextAudioChunk(&achunk, &alen))
                {
                    sendAudioChunk(achunk, alen);
                    sent++;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}
