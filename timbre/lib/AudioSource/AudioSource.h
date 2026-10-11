#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// AudioSource: dueño del microfono PDM integrado del XIAO ESP32-S3 Sense.
//
// Captura a 16kHz 16-bit mono → downsamplea a 8kHz → codifica G.711 u-law.
// Entrega chunks de 160 bytes (20ms de audio a 8kHz) listos para empaquetar
// como RTP payload type 0 (PCMU).
//
// MODO TEST: si se define AUDIO_TEST_MODE (platformio.ini), en vez de leer el
// microfono, entrega en loop el audio embebido de audio_prueba.h. Sirve para
// confirmar que TODO el camino (RTSP -> gateway -> Detective Manager -> ML)
// funciona con audio limpio conocido, aislando si el problema es el microfono.
//
// No es thread-safe (solo se accede desde core 1, el loop principal).
// ---------------------------------------------------------------------------

// Chunk de 20ms: 8000 Hz * 0.020s = 160 samples = 160 bytes u-law
#define AUDIO_CHUNK_SAMPLES 160
// Captura a 16kHz: necesitamos el doble de PCM samples para downsamplear a 8kHz
#define AUDIO_PCM_SAMPLES_PER_CHUNK (AUDIO_CHUNK_SAMPLES * 2)
// Cola de chunks ya codificados entre loop() y el consumidor (RTSP): 40 x 20ms
// = 800 ms. Si se llena, se descarta el mas viejo.
#define AUDIO_QUEUE_CHUNKS 40

class AudioSource
{
public:
    bool begin();
    void end();

    // DRENA todo el buffer DMA (no solo 16 ms) y encola chunks. Llamar en cada
    // vuelta del loop(); el DMA (~768 ms) cubre vueltas lentas (p. ej. la
    // inferencia del PersonDetector).
    // En AUDIO_TEST_MODE no hace nada (nextAudioChunk sirve directo del array).
    void loop();

    // Saca el chunk MAS VIEJO de la cola (G.711 u-law, 160 bytes), en orden.
    // data apunta a un slot INTERNO; valido hasta el proximo loop().
    // Devuelve false si la cola esta vacia. Llamar en un while para vaciarla.
    bool nextAudioChunk(const uint8_t **data, size_t *len);

    // Nivel de amplitud del ultimo chunk (RMS, 0..32767). Para la animacion
    // de "hablando" en la pantalla.
    uint16_t level() const { return _level; }

    bool ready() const { return _ready; }

private:
    bool _ready = false;

    // Buffer de acumulacion PCM (16kHz, 16-bit, mono)
    int16_t _pcmBuf[AUDIO_PCM_SAMPLES_PER_CHUNK];
    size_t _pcmCount = 0; // samples acumulados

    // Cola circular de chunks codificados G.711 u-law (8kHz, 8-bit)
    uint8_t _queue[AUDIO_QUEUE_CHUNKS][AUDIO_CHUNK_SAMPLES];
    size_t _qHead = 0;  // proximo a leer
    size_t _qCount = 0; // chunks en cola
    uint32_t _dropped = 0; // chunks descartados por cola llena

    uint16_t _level = 0; // amplitud RMS del ultimo chunk
    unsigned long _warmupUntilMs = 0; // hasta cuando se descarta el audio tras begin()

    void encodeChunk(); // downsample + u-law encode

#ifdef AUDIO_TEST_MODE
    size_t _testPos = 0;       // posicion actual dentro del array embebido
    unsigned long _lastChunkMs = 0; // para entregar un chunk cada 20ms
#endif
};

extern AudioSource Microphone;
