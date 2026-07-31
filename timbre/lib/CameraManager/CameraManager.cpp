#include "CameraManager.h"

#include <esp_camera.h>
#include "mbedtls/base64.h"

namespace
{
    bool cameraReady = false;

    // Seeed XIAO ESP32S3 Sense camera pins
    #define PWDN_GPIO_NUM     -1
    #define RESET_GPIO_NUM    -1
    #define XCLK_GPIO_NUM     10
    #define SIOD_GPIO_NUM     40
    #define SIOC_GPIO_NUM     39

    #define Y9_GPIO_NUM       48
    #define Y8_GPIO_NUM       11
    #define Y7_GPIO_NUM       12
    #define Y6_GPIO_NUM       14
    #define Y5_GPIO_NUM       16
    #define Y4_GPIO_NUM       18
    #define Y3_GPIO_NUM       17
    #define Y2_GPIO_NUM       15

    #define VSYNC_GPIO_NUM    38
    #define HREF_GPIO_NUM     47
    #define PCLK_GPIO_NUM     13
}

namespace CameraManager
{
    void begin()
    {
        camera_config_t config;
        config.ledc_channel = LEDC_CHANNEL_0;
        config.ledc_timer = LEDC_TIMER_0;
        config.pin_d0 = Y2_GPIO_NUM;
        config.pin_d1 = Y3_GPIO_NUM;
        config.pin_d2 = Y4_GPIO_NUM;
        config.pin_d3 = Y5_GPIO_NUM;
        config.pin_d4 = Y6_GPIO_NUM;
        config.pin_d5 = Y7_GPIO_NUM;
        config.pin_d6 = Y8_GPIO_NUM;
        config.pin_d7 = Y9_GPIO_NUM;
        config.pin_xclk = XCLK_GPIO_NUM;
        config.pin_pclk = PCLK_GPIO_NUM;
        config.pin_vsync = VSYNC_GPIO_NUM;
        config.pin_href = HREF_GPIO_NUM;
        config.pin_sccb_sda = SIOD_GPIO_NUM;
        config.pin_sccb_scl = SIOC_GPIO_NUM;
        config.pin_pwdn = PWDN_GPIO_NUM;
        config.pin_reset = RESET_GPIO_NUM;
        config.xclk_freq_hz = 20000000;
        config.pixel_format = PIXFORMAT_JPEG;

        if (psramFound())
        {
            config.frame_size = FRAMESIZE_QVGA;
            config.jpeg_quality = 12;
            config.fb_count = 1;
            config.fb_location = CAMERA_FB_IN_PSRAM;
            config.grab_mode = CAMERA_GRAB_LATEST;
        }
        else
        {
            config.frame_size = FRAMESIZE_QVGA;
            config.jpeg_quality = 12;
            config.fb_count = 1;
        }

        esp_err_t err = esp_camera_init(&config);
        if (err != ESP_OK)
        {
            Serial.printf("Error inicializando camara: 0x%x\n", err);
            cameraReady = false;
            return;
        }

        cameraReady = true;
        Serial.println("Camara lista");
    }

    String captureImageBase64()
    {
        if (!cameraReady)
        {
            return "";
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            Serial.println("No se pudo capturar imagen");
            return "";
        }

        size_t outputLen = 4 * ((fb->len + 2) / 3);
        char *output = (char *)malloc(outputLen + 1);
        if (!output)
        {
            esp_camera_fb_return(fb);
            Serial.println("No hay memoria para base64");
            return "";
        }

        size_t actualLen = 0;
        int rc = mbedtls_base64_encode(
            (unsigned char *)output,
            outputLen + 1,
            &actualLen,
            fb->buf,
            fb->len);

        esp_camera_fb_return(fb);

        if (rc != 0)
        {
            free(output);
            Serial.printf("Error base64: %d\n", rc);
            return "";
        }

        output[actualLen] = '\0';
        String encoded = String(output);
        free(output);
        return encoded;
    }
}