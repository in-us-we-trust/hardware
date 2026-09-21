#include "AudioSource.h"
#include <driver/i2s.h>
#include <math.h>

#ifdef AUDIO_TEST_MODE
#include "audio_prueba.h" // unsigned char test_ulaw[]; unsigned int test_ulaw_len;
#endif

namespace
{
    const i2s_port_t I2S_PORT = I2S_NUM_0;

    // Pines del microfono PDM INTEGRADO del XIAO ESP32-S3 Sense.
    // En modo PDM RX (core 2.x): el CLK va en ws_io_num, el DATA en data_in_num.
    const int PDM_CLK_PIN = 42;
    const int PDM_DATA_PIN = 41;

    // Ganancia por software: el mic PDM del Sense capta MUY bajo. Los ejemplos
    // oficiales amplifican x16. Ajustar si satura (bajar) o queda flojo (subir).
    const float MIC_GAIN = 16.0f;

    // Al arrancar, el filtro PDM y el seguimiento del DC tiran basura (picos
    // enormes) unos cientos de ms. Ese tiempo se lee y se descarta.
    const uint32_t MIC_WARMUP_MS = 400;

    // G.711 u-law encoder (ITU-T G.711).
    uint8_t pcm16_to_ulaw(int16_t pcm)
    {
        const int16_t CLIP = 32635;
        const int16_t BIAS = 0x84;

        int sign = 0;
        if (pcm < 0) { sign = 0x80; pcm = -pcm; }
        if (pcm > CLIP) pcm = CLIP;
        pcm += BIAS;

        int exponent = 7;
        for (int mask = 0x4000; exponent > 0; exponent--, mask >>= 1)
            if (pcm & mask) break;

        int mantissa = (pcm >> (exponent + 3)) & 0x0F;
        return ~(sign | (exponent << 4) | mantissa);
    }

    inline int16_t applyGain(int16_t s)
    {
        float v = (float)s * MIC_GAIN;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        return (int16_t)v;
    }
}

bool AudioSource::begin()
{
    if (_ready)
        return true;

#ifdef AUDIO_TEST_MODE
    // Modo prueba: no tocamos el I2S real, servimos el audio embebido.
    _testPos = 0;
    _lastChunkMs = 0;
    _ready = true;
    Serial.printf("[MIC] MODO TEST: sirviendo audio embebido (%u bytes, %.1fs)\n",
                  test_ulaw_len, test_ulaw_len / 8000.0f);
    return true;
#else
    // CLAVE: I2S_MODE_PDM en el mode. Sin esto, el core 2.x no decodifica el
    // stream PDM del microfono y sale un zumbido constante.
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // el mic del XIAO S3 esta en el canal RIGHT
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0};

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, nullptr);
    if (err != ESP_OK)
    {
        Serial.printf("[MIC] i2s_driver_install error: %d\n", err);
        return false;
    }

    // En PDM RX: CLK -> ws_io_num, DATA -> data_in_num. El resto NO_CHANGE.
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_PIN_NO_CHANGE,
        .ws_io_num = PDM_CLK_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PDM_DATA_PIN};

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK)
    {
        Serial.printf("[MIC] i2s_set_pin error: %d\n", err);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    // Fijar el clock explicitamente para PDM mono.
    i2s_set_clk(I2S_PORT, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    _pcmCount = 0;
    _chunkReady = false;
    _level = 0;
    _warmupUntilMs = millis() + MIC_WARMUP_MS;
    _ready = true;
    Serial.println("[MIC] PDM listo (16kHz -> G.711 u-law 8kHz, gain x16)");
    return true;
#endif
}

void AudioSource::end()
{
#ifdef AUDIO_TEST_MODE
    _ready = false;
    Serial.println("[MIC] MODO TEST: detenido");
#else
    if (_ready)
    {
        i2s_driver_uninstall(I2S_PORT);
        _ready = false;
        _chunkReady = false;
        _pcmCount = 0;
        _level = 0;
        Serial.println("[MIC] apagado");
    }
#endif
}

void AudioSource::loop()
{
#ifdef AUDIO_TEST_MODE
    return; // en modo test, nextAudioChunk() sirve directo del array embebido
#else
    if (!_ready)
        return;

    int16_t tmpBuf[256];
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_PORT, tmpBuf, sizeof(tmpBuf), &bytesRead, 0);
    if (err != ESP_OK || bytesRead == 0)
        return;

    size_t samplesRead = bytesRead / sizeof(int16_t);

    // El mic PDM tiene un offset DC (~1300). Lo estimamos con un promedio movil
    // y lo restamos, asi el audio queda centrado en 0 antes de amplificar.
    static float dcOffset = 1300.0f;
    bool warmup = (long)(millis() - _warmupUntilMs) < 0;
    for (size_t i = 0; i < samplesRead; i++)
    {
        dcOffset += (tmpBuf[i] - dcOffset) * 0.001f; // seguimiento lento del DC
        if (warmup)
            continue; // descartamos, pero el DC ya va convergiendo
        int16_t centered = (int16_t)(tmpBuf[i] - dcOffset);
        _pcmBuf[_pcmCount++] = applyGain(centered);
        if (_pcmCount >= AUDIO_PCM_SAMPLES_PER_CHUNK)
        {
            encodeChunk();
            _pcmCount = 0;
        }
    }
#endif
}

void AudioSource::encodeChunk()
{
    uint64_t sumSq = 0;
    for (int i = 0; i < AUDIO_CHUNK_SAMPLES; i++)
    {
        int16_t sample = _pcmBuf[i * 2];
        _ulawBuf[i] = pcm16_to_ulaw(sample);
        sumSq += (int32_t)sample * (int32_t)sample;
    }
    _level = (uint16_t)sqrt((double)(sumSq / AUDIO_CHUNK_SAMPLES));
    _chunkReady = true;
}

bool AudioSource::nextAudioChunk(const uint8_t **data, size_t *len)
{
#ifdef AUDIO_TEST_MODE
    if (!_ready)
        return false;

    // Un chunk cada 20ms (mismo ritmo que el micrografo real, 160 bytes @ 8kHz).
    unsigned long now = millis();
    if (now - _lastChunkMs < 20)
        return false;
    _lastChunkMs = now;

    if (_testPos + AUDIO_CHUNK_SAMPLES > test_ulaw_len)
        _testPos = 0; // loop: volver al principio del audio de prueba

    *data = &test_ulaw[_testPos];
    *len = AUDIO_CHUNK_SAMPLES;
    _testPos += AUDIO_CHUNK_SAMPLES;

    // Nivel aproximado para la animacion (u-law: 0x7F/0xFF ~ silencio digital)
    uint32_t sum = 0;
    for (int i = 0; i < AUDIO_CHUNK_SAMPLES; i++)
        sum += abs((int)test_ulaw[_testPos - AUDIO_CHUNK_SAMPLES + i] - 0xFF);
    _level = (uint16_t)(sum * 20); // escalado grosero, solo para la barra

    return true;
#else
    if (!_chunkReady)
        return false;
    *data = _ulawBuf;
    *len = AUDIO_CHUNK_SAMPLES;
    _chunkReady = false;
    return true;
#endif
}

AudioSource Microphone;
