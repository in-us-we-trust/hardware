#include "AudioSource.h"
#include <driver/i2s.h>

namespace
{
    const i2s_port_t I2S_PORT = I2S_NUM_0;

    // Pines del microfono PDM INTEGRADO del XIAO ESP32-S3 Sense
    // (MSM261S4030H0R). NO son los pines I2S del AudioManager viejo.
    #define PDM_CLK_PIN 42
    #define PDM_DATA_PIN 41

    // -----------------------------------------------------------------------
    // G.711 u-law encoder (ITU-T G.711). Convierte un sample PCM 16-bit
    // signed a un byte u-law. Es el codec estandar de telefonia; FFmpeg lo
    // decodifica nativo con payload type 0 (PCMU).
    // -----------------------------------------------------------------------
    uint8_t pcm16_to_ulaw(int16_t pcm)
    {
        const int16_t CLIP = 32635;
        const int16_t BIAS = 0x84;

        int sign = 0;
        if (pcm < 0)
        {
            sign = 0x80;
            pcm = -pcm;
        }
        if (pcm > CLIP)
            pcm = CLIP;
        pcm += BIAS;

        // Encontrar el exponente (segmento)
        int exponent = 7;
        for (int mask = 0x4000; exponent > 0; exponent--, mask >>= 1)
        {
            if (pcm & mask)
                break;
        }

        int mantissa = (pcm >> (exponent + 3)) & 0x0F;
        uint8_t ulaw = ~(sign | (exponent << 4) | mantissa);
        return ulaw;
    }
}

bool AudioSource::begin()
{
    if (_ready)
        return true;

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0};

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_PIN_NO_CHANGE, // PDM no usa BCK
        .ws_io_num = PDM_CLK_PIN,        // PDM CLK
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PDM_DATA_PIN};    // PDM DATA

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, nullptr);
    if (err != ESP_OK)
    {
        Serial.printf("[MIC] i2s_driver_install error: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK)
    {
        Serial.printf("[MIC] i2s_set_pin error: %d\n", err);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    _pcmCount = 0;
    _chunkReady = false;
    _ready = true;
    Serial.println("[MIC] PDM listo (16kHz → G.711 u-law 8kHz)");
    return true;
}

void AudioSource::end()
{
    if (_ready)
    {
        i2s_driver_uninstall(I2S_PORT);
        _ready = false;
        _chunkReady = false;
        _pcmCount = 0;
        Serial.println("[MIC] apagado");
    }
}

void AudioSource::loop()
{
    if (!_ready)
        return;

    // Leemos lo que haya acumulado el DMA (timeout 0 = no bloqueante).
    // Cada sample es int16_t (2 bytes).
    int16_t tmpBuf[256];
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_PORT, tmpBuf, sizeof(tmpBuf), &bytesRead, 0);
    if (err != ESP_OK || bytesRead == 0)
        return;

    size_t samplesRead = bytesRead / sizeof(int16_t);

    // Acumular en _pcmBuf hasta tener AUDIO_PCM_SAMPLES_PER_CHUNK (320 samples)
    for (size_t i = 0; i < samplesRead; i++)
    {
        _pcmBuf[_pcmCount++] = tmpBuf[i];

        if (_pcmCount >= AUDIO_PCM_SAMPLES_PER_CHUNK)
        {
            encodeChunk();
            _pcmCount = 0;
        }
    }
}

// Downsamplea 320 samples PCM 16kHz → 160 samples 8kHz, codifica a u-law,
// y calcula el nivel RMS para la animacion.
void AudioSource::encodeChunk()
{
    uint64_t sumSq = 0;
    for (int i = 0; i < AUDIO_CHUNK_SAMPLES; i++)
    {
        // Downsample 2:1 (tomar cada segundo sample)
        int16_t sample = _pcmBuf[i * 2];
        _ulawBuf[i] = pcm16_to_ulaw(sample);
        sumSq += (int32_t)sample * (int32_t)sample;
    }
    _level = (uint16_t)sqrt((double)(sumSq / AUDIO_CHUNK_SAMPLES));
    _chunkReady = true;
}

bool AudioSource::nextAudioChunk(const uint8_t **data, size_t *len)
{
    if (!_chunkReady)
        return false;

    *data = _ulawBuf;
    *len = AUDIO_CHUNK_SAMPLES; // 160 bytes = 20ms de audio
    _chunkReady = false;
    return true;
}

// Instancia global: unico dueño del microfono.
AudioSource Microphone;
