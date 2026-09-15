#include "AudioSource.h"
#include <driver/i2s.h>
#include <math.h>

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

    // G.711 u-law encoder (ITU-T G.711).
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

        int exponent = 7;
        for (int mask = 0x4000; exponent > 0; exponent--, mask >>= 1)
            if (pcm & mask)
                break;

        int mantissa = (pcm >> (exponent + 3)) & 0x0F;
        return ~(sign | (exponent << 4) | mantissa);
    }

    inline int16_t applyGain(int16_t s)
    {
        float v = (float)s * MIC_GAIN;
        if (v > 32767.0f)
            v = 32767.0f;
        if (v < -32768.0f)
            v = -32768.0f;
        return (int16_t)v;
    }
}

bool AudioSource::begin()
{
    if (_ready)
        return true;

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
    _ready = true;
    Serial.println("[MIC] PDM listo (16kHz -> G.711 u-law 8kHz, gain x16)");
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

    int16_t tmpBuf[256];
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_PORT, tmpBuf, sizeof(tmpBuf), &bytesRead, 0);
    if (err != ESP_OK || bytesRead == 0)
        return;

    size_t samplesRead = bytesRead / sizeof(int16_t);

    // El mic PDM tiene un offset DC (~1300). Lo estimamos con un promedio movil
    // y lo restamos, asi el audio queda centrado en 0 antes de amplificar.
    static float dcOffset = 1300.0f;
    for (size_t i = 0; i < samplesRead; i++)
    {
        dcOffset += (tmpBuf[i] - dcOffset) * 0.001f; // seguimiento lento del DC
        int16_t centered = (int16_t)(tmpBuf[i] - dcOffset);
        _pcmBuf[_pcmCount++] = applyGain(centered);
        if (_pcmCount >= AUDIO_PCM_SAMPLES_PER_CHUNK)
        {
            encodeChunk();
            _pcmCount = 0;
        }
    }
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
    if (!_chunkReady)
        return false;
    *data = _ulawBuf;
    *len = AUDIO_CHUNK_SAMPLES;
    _chunkReady = false;
    return true;
}

AudioSource Microphone;
