#include "AudioManager.h"

#include <driver/i2s.h>

namespace
{
    const i2s_port_t I2S_PORT = I2S_NUM_0;

    bool audioReady = false;

    void setupI2S(uint32_t sampleRate, i2s_bits_per_sample_t bits, i2s_channel_fmt_t channelFmt)
    {
        i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
            .sample_rate = sampleRate,
            .bits_per_sample = bits,
            .channel_format = channelFmt,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 8,
            .dma_buf_len = 128,
            .use_apll = false,
            .tx_desc_auto_clear = true,
            .fixed_mclk = 0
        };

        i2s_pin_config_t pin_config = {
            .mck_io_num = I2S_PIN_NO_CHANGE,
            .bck_io_num = 6,
            .ws_io_num = 4,
            .data_out_num = 8,
            .data_in_num = 7
        };

        esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, nullptr);
        if (err != ESP_OK)
        {
            Serial.printf("[AudioManager] i2s_driver_install error: %d\n", err);
            return;
        }

        err = i2s_set_pin(I2S_PORT, &pin_config);
        if (err != ESP_OK)
        {
            Serial.printf("[AudioManager] i2s_set_pin error: %d\n", err);
            i2s_driver_uninstall(I2S_PORT);
            return;
        }

        //i2s_set_clk(I2S_PORT, sampleRate, bits, I2S_CHANNEL_STEREO);
        i2s_set_clk(I2S_PORT, sampleRate, bits, I2S_CHANNEL_MONO);
        audioReady = true;
        Serial.println("[AudioManager] I2S listo");
    }
}

namespace AudioManager
{
    void begin()
    {
        setupI2S(16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_FMT_ONLY_LEFT);
    }

    void beginInput()
    {
        if (audioReady)
            return;
        begin();
    }

    void beginOutput()
    {
        if (audioReady)
            return;
        begin();
    }

    void end()
    {
        if (audioReady)
        {
            i2s_driver_uninstall(I2S_PORT);
            audioReady = false;
            Serial.println("[AudioManager] I2S apagado");
        }
    }

    void loop()
    {
        if (!audioReady)
            return;

        int16_t buffer[128] = {0};
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_PORT, buffer, sizeof(buffer), &bytesRead, 0);
        if (err == ESP_OK && bytesRead > 0)
        {
            // placeholder: el flujo real de audio se conectará más adelante al stream WebRTC.
            // aqui solo se mantiene el canal abierto para no romper la pila I2S.
        }
    }

    bool isReady()
    {
        return audioReady;
    }
}
