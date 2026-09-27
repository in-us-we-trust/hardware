#pragma once
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// MediaSource: interfaz de la fuente de media que consume el RtspPublisher.
// Desacopla "de donde salen los bytes" de "como se publican por RTSP".
//
// Implementaciones intercambiables:
//   - MockMediaSource   -> JPEG/WAV embebidos (desarrollo, Wokwi, sin hardware)
//   - CameraMediaSource -> OV2640 + mic PDM reales (fase C, con la XIAO)
//
// El RtspPublisher habla SIEMPRE con esta interfaz y no sabe cual esta enchufada.
// Es una interfaz (clase abstracta), no un namespace, justamente porque hay
// varias implementaciones que se eligen segun el entorno.
// ---------------------------------------------------------------------------

class MediaSource
{
public:
    virtual ~MediaSource() {}

    // Prepara la fuente (abrir camara, cargar el mock, etc.). true si ok.
    virtual bool begin() = 0;

    // Entrega el proximo frame de VIDEO como JPEG.
    //   data: puntero al buffer del frame (propiedad de la fuente; NO liberar).
    //   len:  tamaño en bytes.
    // Devuelve false si no hay frame disponible en este momento.
    virtual bool nextVideoFrame(const uint8_t **data, size_t *len) = 0;

    // Igual que nextVideoFrame pero COPIA el frame al buffer del caller.
    // Usar esto cuando el envio es lento (RTSP/TLS): con nextVideoFrame el
    // buffer interno puede ser pisado por otra captura (task MJPEG en el otro
    // core) mientras se manda. Default: copia lo que da nextVideoFrame (sirve
    // para fuentes sin concurrencia, como el mock). CameraSource lo hace bajo mutex.
    virtual bool copyVideoFrame(uint8_t *dest, size_t destCap, size_t *outLen)
    {
        const uint8_t *data = nullptr;
        size_t len = 0;
        if (!nextVideoFrame(&data, &len) || len > destCap)
            return false;
        memcpy(dest, data, len);
        *outLen = len;
        return true;
    }

    // FPS objetivo del video (marca la cadencia con la que el publisher pide).
    virtual int videoFps() const = 0;

    // --- Audio: fase B ---
    // (se agregara nextAudioChunk(...) con PCM 16kHz mono cuando sumemos audio)
};
