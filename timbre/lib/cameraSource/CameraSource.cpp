#include "CameraSource.h"

#include <esp_camera.h>
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

namespace
{
    // Seeed XIAO ESP32S3 Sense camera pins
    #define PWDN_GPIO_NUM  -1
    #define RESET_GPIO_NUM -1
    #define XCLK_GPIO_NUM  10
    #define SIOD_GPIO_NUM  40
    #define SIOC_GPIO_NUM  39

    #define Y9_GPIO_NUM 48
    #define Y8_GPIO_NUM 11
    #define Y7_GPIO_NUM 12
    #define Y6_GPIO_NUM 14
    #define Y5_GPIO_NUM 16
    #define Y4_GPIO_NUM 18
    #define Y3_GPIO_NUM 17
    #define Y2_GPIO_NUM 15

    #define VSYNC_GPIO_NUM 38
    #define HREF_GPIO_NUM  47
    #define PCLK_GPIO_NUM  13
}

bool CameraSource::begin()
{
    if (_ready)
        return true; // ya inicializada

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
        config.fb_count = 2; // stream local + RTSP piden frames
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
    }
    else
    {
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 2;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.printf("[CAM] error inicializando: 0x%x\n", err);
        _ready = false;
        return false;
    }

    // Ajustes del sensor (orientacion, etc.). Si la imagen sale de cabeza,
    // descomentar el vflip / hmirror:
    // sensor_t *s = esp_camera_sensor_get();
    // if (s) { s->set_vflip(s, 1); s->set_hmirror(s, 1); }

    _ready = true;
    Serial.println("[CAM] lista");
    return true;
}

void CameraSource::end()
{
    if (_ready)
    {
        esp_camera_deinit();
        _ready = false;
        Serial.println("[CAM] apagada");
    }
    if (_buf)
    {
        free(_buf); // libera la copia para no retener PSRAM en reposo
        _buf = nullptr;
        _cap = 0;
        _len = 0;
    }
}

bool CameraSource::captureIntoBuffer()
{
    if (!_ready)
        return false;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
        Serial.println("[CAM] no se pudo capturar");
        return false;
    }

    // Aseguramos capacidad y copiamos, para devolver el fb enseguida.
    if (_cap < fb->len)
    {
        uint8_t *nb = (uint8_t *)(psramFound()
                                      ? heap_caps_realloc(_buf, fb->len, MALLOC_CAP_SPIRAM)
                                      : realloc(_buf, fb->len));
        if (!nb)
        {
            esp_camera_fb_return(fb);
            Serial.println("[CAM] sin memoria para la copia");
            return false;
        }
        _buf = nb;
        _cap = fb->len;
    }

    memcpy(_buf, fb->buf, fb->len);
    _len = fb->len;
    esp_camera_fb_return(fb); // devolvemos el framebuffer de inmediato
    return true;
}

bool CameraSource::nextVideoFrame(const uint8_t **data, size_t *len)
{
    if (!captureIntoBuffer())
        return false;
    *data = _buf;
    *len = _len;
    return true;
}

String CameraSource::captureBase64()
{
    if (!captureIntoBuffer())
        return "";

    size_t outputLen = 4 * ((_len + 2) / 3);
    char *output = (char *)malloc(outputLen + 1);
    if (!output)
    {
        Serial.println("[CAM] sin memoria para base64");
        return "";
    }

    size_t actualLen = 0;
    int rc = mbedtls_base64_encode(
        (unsigned char *)output, outputLen + 1, &actualLen, _buf, _len);
    if (rc != 0)
    {
        free(output);
        Serial.printf("[CAM] error base64: %d\n", rc);
        return "";
    }

    output[actualLen] = '\0';
    String encoded = String(output);
    free(output);
    return encoded;
}

// Instancia global: unico dueño de la camara. 5 fps objetivo para el RTSP.
CameraSource Camera(5);
