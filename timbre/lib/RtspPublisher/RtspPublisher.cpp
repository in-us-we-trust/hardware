#include "RtspPublisher.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <string.h>

#ifndef MEDIAMTX_PORT
#define MEDIAMTX_PORT 8554
#endif

#define RTP_PAYLOAD_TYPE 26 // JPEG (RFC 2435)
#define RTP_CHANNEL_VIDEO 0 // canal interleaved para RTP de video

// MTU efectivo de MediaMTX: avisa "RTP packets too big (>1440)". Nos mantenemos
// comodos por debajo. Cada paquete RTP completo <= MAX_RTP_PACKET.
#define MAX_RTP_PACKET 1400

namespace
{
    using State = RtspPublisher::State;

    WiFiClient tcp;
    MediaSource *src = nullptr;

    String mtxHost;
    String mtxPath = "doorbell";

    State st = State::Idle;
    uint32_t cseq = 1;
    String sessionId;

    uint16_t rtpSeq = 0;
    uint32_t rtpSsrc = 0;
    uint32_t rtpTimestamp = 0;
    uint32_t frameCount = 0;

    unsigned long lastFrameMs = 0;

    // Buffer de armado: 4 bytes de framing interleaved + el paquete RTP.
    uint8_t pkt[MAX_RTP_PACKET + 8];

    String rtspUrl()
    {
        return "rtsp://" + mtxHost + ":" + String(MEDIAMTX_PORT) + "/" + mtxPath;
    }

    // --- Handshake RTSP (sin cambios respecto al esqueleto) ----------------
    void sendRequest(const String &method, const String &extraHeaders,
                     const String &body = "")
    {
        String req = method + " " + rtspUrl() + " RTSP/1.0\r\n";
        req += "CSeq: " + String(cseq++) + "\r\n";
        req += "User-Agent: timbre-esp32\r\n";
        if (sessionId.length() > 0) req += "Session: " + sessionId + "\r\n";
        req += extraHeaders;
        if (body.length() > 0) req += "Content-Length: " + String(body.length()) + "\r\n";
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
                if (resp.endsWith("\r\n\r\n")) return resp;
            }
            delay(1);
        }
        return resp;
    }

    bool responseOk(const String &resp) { return resp.startsWith("RTSP/1.0 200"); }

    String buildSdp()
    {
        String sdp;
        sdp += "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 127.0.0.1\r\n";
        sdp += "s=timbre\r\n";
        sdp += "c=IN IP4 0.0.0.0\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=video 0 RTP/AVP 26\r\n";
        sdp += "a=rtpmap:26 JPEG/90000\r\n";
        sdp += "a=control:streamid=0\r\n";
        return sdp;
    }

    void fail(const char *why)
    {
        Serial.printf("[RTSP] error: %s\n", why);
        st = State::Error;
        tcp.stop();
    }

    // -----------------------------------------------------------------------
    // Parser JPEG minimo para RFC 2435: extrae dimensiones, subsampling, las
    // dos tablas de cuantizacion y el rango del scan data (datos entropicos).
    // -----------------------------------------------------------------------
    struct JpegInfo
    {
        uint16_t width = 0, height = 0;
        uint8_t type = 0;          // 0 = 4:2:2, 1 = 4:2:0
        const uint8_t *qt[2] = {nullptr, nullptr}; // luma, chroma (64 bytes c/u)
        bool haveQt[2] = {false, false};
        size_t scanStart = 0, scanLen = 0;
        bool hasDri = false;
        bool ok = false;
    };

    JpegInfo parseJpeg(const uint8_t *d, size_t len)
    {
        JpegInfo j;
        if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return j; // SOI
        size_t p = 2;
        while (p + 4 <= len)
        {
            if (d[p] != 0xFF) { p++; continue; }
            uint8_t marker = d[p + 1];
            if (marker == 0xD9) break;                                   // EOI
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))    // standalone
            {
                p += 2;
                continue;
            }
            uint16_t seglen = (d[p + 2] << 8) | d[p + 3];
            size_t segStart = p + 4;
            size_t segEnd = p + 2 + seglen;
            if (segEnd > len) break;

            if (marker == 0xDB) // DQT
            {
                size_t q = segStart;
                while (q < segEnd)
                {
                    uint8_t pqtq = d[q++];
                    uint8_t pq = pqtq >> 4;   // precision (0 = 8 bits)
                    uint8_t tq = pqtq & 0x0F; // id de tabla
                    if (pq == 0 && tq < 2 && q + 64 <= segEnd)
                    {
                        j.qt[tq] = &d[q];
                        j.haveQt[tq] = true;
                        q += 64;
                    }
                    else break; // tablas de 16 bits no soportadas
                }
            }
            else if (marker == 0xC0) // SOF0 (baseline)
            {
                j.height = (d[segStart + 1] << 8) | d[segStart + 2];
                j.width = (d[segStart + 3] << 8) | d[segStart + 4];
                uint8_t nc = d[segStart + 5];
                if (nc >= 1)
                {
                    uint8_t samp = d[segStart + 7]; // sampling del componente Y
                    uint8_t v = samp & 0x0F;
                    j.type = (v == 2) ? 1 : 0; // vertical x2 => 4:2:0
                }
            }
            else if (marker == 0xC2) // progresivo: RFC2435 es baseline
            {
                return j; // ok = false
            }
            else if (marker == 0xDD) // DRI (restart interval)
            {
                j.hasDri = true; // no soportado en v1 (ver nota abajo)
            }
            else if (marker == 0xDA) // SOS -> arranca el scan data
            {
                j.scanStart = segEnd;
                size_t end = len;
                if (len >= 2 && d[len - 2] == 0xFF && d[len - 1] == 0xD9)
                    end = len - 2; // sacar EOI
                j.scanLen = end - j.scanStart;
                j.ok = j.haveQt[0] && j.haveQt[1] && !j.hasDri;
                return j;
            }
            p = segEnd;
        }
        return j;
    }

    // Envia un paquete RTP con el framing interleaved TCP ('$' + canal + len).
    bool sendInterleaved(uint8_t *buf, uint16_t rtpLen)
    {
        buf[0] = 0x24; // '$'
        buf[1] = RTP_CHANNEL_VIDEO;
        buf[2] = rtpLen >> 8;
        buf[3] = rtpLen & 0xFF;
        size_t total = 4 + rtpLen;
        return tcp.write(buf, total) == total;
    }

    // -----------------------------------------------------------------------
    // Empaqueta un frame JPEG como RTP-JPEG (RFC 2435) y lo envia fragmentado.
    // Primer paquete: RTP(12) + JPEG hdr(8) + quant hdr(4) + tablas(128) + scan.
    // Siguientes:     RTP(12) + JPEG hdr(8) + scan. Marker bit en el ultimo.
    // -----------------------------------------------------------------------
    void sendVideoFrame(const uint8_t *jpeg, size_t len)
    {
        JpegInfo j = parseJpeg(jpeg, len);
        if (!j.ok)
        {
            Serial.println("[RTSP] JPEG no apto (sin 2 tablas / progresivo / DRI): frame descartado");
            return;
        }
        if ((j.width / 8) > 255 || (j.height / 8) > 255)
        {
            Serial.println("[RTSP] dimensiones fuera de rango RFC2435");
            return;
        }

        int fps = src->videoFps() > 0 ? src->videoFps() : 10;
        rtpTimestamp += 90000 / fps; // mismo timestamp para todos los fragmentos

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

            uint8_t *w = pkt + 4; // dejamos pkt[0..3] para el framing

            // RTP header (12)
            w[0] = 0x80;
            w[1] = (last ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE; // marker en el ultimo
            w[2] = rtpSeq >> 8;
            w[3] = rtpSeq & 0xFF;
            rtpSeq++;
            w[4] = rtpTimestamp >> 24;
            w[5] = rtpTimestamp >> 16;
            w[6] = rtpTimestamp >> 8;
            w[7] = rtpTimestamp & 0xFF;
            w[8] = rtpSsrc >> 24;
            w[9] = rtpSsrc >> 16;
            w[10] = rtpSsrc >> 8;
            w[11] = rtpSsrc & 0xFF;
            w += 12;

            // JPEG main header (8)
            w[0] = 0;                     // type-specific
            w[1] = (offset >> 16) & 0xFF; // fragment offset (24 bits)
            w[2] = (offset >> 8) & 0xFF;
            w[3] = offset & 0xFF;
            w[4] = j.type;      // 0 = 4:2:2, 1 = 4:2:0
            w[5] = 255;         // Q = 255 -> tablas dinamicas incluidas
            w[6] = j.width / 8;
            w[7] = j.height / 8;
            w += 8;

            // Quantization table header + tablas (solo primer paquete)
            if (first)
            {
                w[0] = 0;   // MBZ
                w[1] = 0;   // precision (8 bits)
                w[2] = 0;   // length hi
                w[3] = 128; // length lo (2 tablas de 64)
                w += 4;
                memcpy(w, j.qt[0], 64);
                w += 64;
                memcpy(w, j.qt[1], 64);
                w += 64;
            }

            // scan data
            memcpy(w, scan + offset, chunk);
            w += chunk;

            uint16_t rtpLen = (w - (pkt + 4));
            if (!sendInterleaved(pkt, rtpLen))
            {
                fail("write de paquete RTP fallo");
                return;
            }

            offset += chunk;
            remaining -= chunk;
        }

        frameCount++;
        if (frameCount % 30 == 0) // log cada ~3s a 10fps
        {
            Serial.printf("[RTSP] enviados %u frames (ultimo %u bytes)\n",
                          frameCount, (unsigned)len);
        }
    }
}

namespace RtspPublisher
{
    void begin(const String &mediaMtxHost, MediaSource *source, const String &path)
    {
        mtxHost = mediaMtxHost;
        mtxPath = path;
        src = source;
        st = State::Idle;
        rtpSsrc = esp_random();
        Serial.printf("[RTSP] configurado -> %s\n", rtspUrl().c_str());
    }

    bool start()
    {
        if (src == nullptr) { fail("sin MediaSource"); return false; }
        if (!src->begin()) { fail("MediaSource::begin fallo"); return false; }

        Serial.printf("[RTSP] conectando a %s:%d...\n", mtxHost.c_str(), MEDIAMTX_PORT);
        if (!tcp.connect(mtxHost.c_str(), MEDIAMTX_PORT))
        {
            fail("no se pudo abrir TCP a MediaMTX");
            return false;
        }

        cseq = 1;
        sessionId = "";
        frameCount = 0;
        st = State::Announcing;
        sendRequest("ANNOUNCE", "Content-Type: application/sdp\r\n", buildSdp());
        return true;
    }

    void stop()
    {
        if (st == State::Idle) return;
        if (tcp.connected()) sendRequest("TEARDOWN", "");
        tcp.stop();
        st = State::Idle;
        Serial.println("[RTSP] detenido (TEARDOWN)");
    }

    bool isPublishing() { return st == State::Publishing; }
    String streamUrl() { return rtspUrl(); }
    State state() { return st; }

    void loop()
    {
        switch (st)
        {
        case State::Announcing:
        {
            if (!tcp.available()) return;
            String resp = readResponse();
            if (!responseOk(resp)) { fail("ANNOUNCE no-200"); return; }
            st = State::SettingUp;
            String url = rtspUrl() + "/streamid=0";
            String setup = "SETUP " + url + " RTSP/1.0\r\n";
            setup += "CSeq: " + String(cseq++) + "\r\n";
            setup += "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n";
            setup += "\r\n";
            tcp.print(setup);
            Serial.println("[RTSP] -> SETUP (interleaved TCP, mode=record)");
            break;
        }
        case State::SettingUp:
        {
            if (!tcp.available()) return;
            String resp = readResponse();
            if (!responseOk(resp)) { fail("SETUP no-200"); return; }
            int idx = resp.indexOf("Session:");
            if (idx >= 0)
            {
                int nl = resp.indexOf("\r\n", idx);
                sessionId = resp.substring(idx + 8, nl);
                sessionId.trim();
                int semi = sessionId.indexOf(';');
                if (semi >= 0) sessionId = sessionId.substring(0, semi);
                sessionId.trim();
            }
            st = State::Recording;
            sendRequest("RECORD", "Range: npt=0.000-\r\n");
            break;
        }
        case State::Recording:
        {
            if (!tcp.available()) return;
            String resp = readResponse();
            if (!responseOk(resp)) { fail("RECORD no-200"); return; }
            st = State::Publishing;
            lastFrameMs = 0;
            Serial.println("[RTSP] PUBLICANDO (handshake completo)");
            break;
        }
        case State::Publishing:
        {
            if (!tcp.connected()) { fail("TCP caido durante Publishing"); return; }
            int fps = src->videoFps() > 0 ? src->videoFps() : 10;
            unsigned long interval = 1000 / fps;
            if (millis() - lastFrameMs >= interval)
            {
                lastFrameMs = millis();
                const uint8_t *data;
                size_t flen;
                if (src->nextVideoFrame(&data, &flen))
                {
                    sendVideoFrame(data, flen);
                }
            }
            break;
        }
        default:
            break;
        }
    }
}
