#include "PersonDetector.h"

#include "person_detect_model_data.h"

#include <Chirale_TensorFlowLite.h>
#include <esp_heap_caps.h>
#include <img_converters.h>
#include <math.h>
#include <new>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>

namespace
{
    constexpr int kInputWidth = 96;
    constexpr int kInputHeight = 96;
    constexpr int kInputChannels = 1;
    constexpr int kPersonIndex = 1;
    constexpr int kNoPersonIndex = 0;
    constexpr float kPersonThreshold = 0.40f;
    constexpr size_t kTensorArenaSize = 160 * 1024;

    const tflite::Model *model = nullptr;
    tflite::MicroInterpreter *interpreter = nullptr;
    TfLiteTensor *input = nullptr;
    TfLiteTensor *output = nullptr;
    uint8_t *tensorArena = nullptr;
    bool ready = false;

    bool isStartOfFrameMarker(uint8_t marker)
    {
        switch (marker)
        {
        case 0xC0:
        case 0xC1:
        case 0xC2:
        case 0xC3:
        case 0xC5:
        case 0xC6:
        case 0xC7:
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCD:
        case 0xCE:
        case 0xCF:
            return true;
        default:
            return false;
        }
    }

    bool readJpegDimensions(const uint8_t *data, size_t length,
                            uint16_t &width, uint16_t &height)
    {
        if (data == nullptr || length < 4 || data[0] != 0xFF || data[1] != 0xD8)
        {
            return false;
        }

        size_t position = 2;
        while (position + 1 < length)
        {
            if (data[position] != 0xFF)
            {
                ++position;
                continue;
            }

            while (position < length && data[position] == 0xFF)
            {
                ++position;
            }
            if (position >= length)
            {
                return false;
            }

            const uint8_t marker = data[position++];

            // Marcadores sin payload.
            if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
                (marker >= 0xD0 && marker <= 0xD7))
            {
                continue;
            }

            // El SOF siempre debe aparecer antes de comenzar los scan data.
            if (marker == 0xDA || position + 2 > length)
            {
                return false;
            }

            const uint16_t segmentLength =
                (static_cast<uint16_t>(data[position]) << 8) |
                static_cast<uint16_t>(data[position + 1]);
            if (segmentLength < 2 || position + segmentLength > length)
            {
                return false;
            }

            if (isStartOfFrameMarker(marker))
            {
                if (segmentLength < 7)
                {
                    return false;
                }

                height = (static_cast<uint16_t>(data[position + 3]) << 8) |
                         static_cast<uint16_t>(data[position + 4]);
                width = (static_cast<uint16_t>(data[position + 5]) << 8) |
                        static_cast<uint16_t>(data[position + 6]);
                return width > 0 && height > 0;
            }

            position += segmentLength;
        }

        return false;
    }

    uint8_t grayscaleAt(const uint8_t *rgb, uint16_t width, int x, int y)
    {
        const size_t index =
            (static_cast<size_t>(y) * static_cast<size_t>(width) + x) * 3;
        const uint8_t red = rgb[index];
        const uint8_t green = rgb[index + 1];
        const uint8_t blue = rgb[index + 2];

        // Aproximacion entera de 0.299R + 0.587G + 0.114B.
        return static_cast<uint8_t>(
            (77 * red + 150 * green + 29 * blue + 128) >> 8);
    }

    int8_t quantizeGrayscale(float grayscale, const TfLiteTensor *tensor)
    {
        long quantized = lroundf(grayscale / tensor->params.scale) +
                         tensor->params.zero_point;
        if (quantized < -128)
            quantized = -128;
        if (quantized > 127)
            quantized = 127;
        return static_cast<int8_t>(quantized);
    }

    bool resizeCenterCropToInput(const uint8_t *rgb, uint16_t width,
                                 uint16_t height)
    {
        if (rgb == nullptr || input == nullptr || input->params.scale <= 0.0f)
        {
            return false;
        }

        const int cropSize = width < height ? width : height;
        const int cropX = (static_cast<int>(width) - cropSize) / 2;
        const int cropY = (static_cast<int>(height) - cropSize) / 2;
        const float scaleX = static_cast<float>(cropSize) / kInputWidth;
        const float scaleY = static_cast<float>(cropSize) / kInputHeight;

        for (int targetY = 0; targetY < kInputHeight; ++targetY)
        {
            float sourceY = cropY + (targetY + 0.5f) * scaleY - 0.5f;
            if (sourceY < cropY)
                sourceY = cropY;
            const float maxY = cropY + cropSize - 1;
            if (sourceY > maxY)
                sourceY = maxY;

            const int y0 = static_cast<int>(floorf(sourceY));
            const int y1 = y0 + 1 < cropY + cropSize ? y0 + 1 : y0;
            const float yWeight = sourceY - y0;

            for (int targetX = 0; targetX < kInputWidth; ++targetX)
            {
                float sourceX = cropX + (targetX + 0.5f) * scaleX - 0.5f;
                if (sourceX < cropX)
                    sourceX = cropX;
                const float maxX = cropX + cropSize - 1;
                if (sourceX > maxX)
                    sourceX = maxX;

                const int x0 = static_cast<int>(floorf(sourceX));
                const int x1 = x0 + 1 < cropX + cropSize ? x0 + 1 : x0;
                const float xWeight = sourceX - x0;

                const float top =
                    grayscaleAt(rgb, width, x0, y0) * (1.0f - xWeight) +
                    grayscaleAt(rgb, width, x1, y0) * xWeight;
                const float bottom =
                    grayscaleAt(rgb, width, x0, y1) * (1.0f - xWeight) +
                    grayscaleAt(rgb, width, x1, y1) * xWeight;
                const float grayscale =
                    top * (1.0f - yWeight) + bottom * yWeight;

                input->data.int8[targetY * kInputWidth + targetX] =
                    quantizeGrayscale(grayscale, input);
            }
        }

        return true;
    }

    float dequantize(int8_t value, const TfLiteTensor *tensor)
    {
        return (static_cast<int32_t>(value) - tensor->params.zero_point) *
               tensor->params.scale;
    }

    void releaseInterpreter()
    {
        delete interpreter;
        interpreter = nullptr;
        input = nullptr;
        output = nullptr;
        model = nullptr;
        ready = false;

        if (tensorArena != nullptr)
        {
            heap_caps_free(tensorArena);
            tensorArena = nullptr;
        }
    }
}

namespace PersonDetector
{
    bool begin()
    {
        if (ready)
        {
            return true;
        }

        Serial.println("[PersonDetector] Inicializando TFLite Micro...");

        model = tflite::GetModel(g_person_detect_model_data);
        if (model == nullptr || model->version() != TFLITE_SCHEMA_VERSION)
        {
            Serial.printf(
                "[PersonDetector] Schema incompatible: modelo=%d runtime=%d\n",
                model == nullptr ? -1 : model->version(), TFLITE_SCHEMA_VERSION);
            releaseInterpreter();
            return false;
        }

        tensorArena = static_cast<uint8_t *>(heap_caps_malloc(
            kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (tensorArena == nullptr)
        {
            Serial.println(
                "[PersonDetector] PSRAM no disponible; probando RAM interna...");
            tensorArena = static_cast<uint8_t *>(
                heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_8BIT));
        }
        if (tensorArena == nullptr)
        {
            Serial.printf(
                "[PersonDetector] No se pudo reservar tensor arena (%u bytes)\n",
                static_cast<unsigned>(kTensorArenaSize));
            releaseInterpreter();
            return false;
        }

        static tflite::MicroMutableOpResolver<5> resolver;
        static bool resolverConfigured = false;
        if (!resolverConfigured)
        {
            if (resolver.AddAveragePool2D() != kTfLiteOk ||
                resolver.AddConv2D() != kTfLiteOk ||
                resolver.AddDepthwiseConv2D() != kTfLiteOk ||
                resolver.AddReshape() != kTfLiteOk ||
                resolver.AddSoftmax() != kTfLiteOk)
            {
                Serial.println("[PersonDetector] No se pudieron registrar los operadores");
                releaseInterpreter();
                return false;
            }
            resolverConfigured = true;
        }

        interpreter = new (std::nothrow) tflite::MicroInterpreter(
            model, resolver, tensorArena, kTensorArenaSize);
        if (interpreter == nullptr)
        {
            Serial.println("[PersonDetector] No se pudo crear el interprete");
            releaseInterpreter();
            return false;
        }

        if (interpreter->AllocateTensors() != kTfLiteOk)
        {
            Serial.println("[PersonDetector] AllocateTensors() fallo");
            releaseInterpreter();
            return false;
        }

        input = interpreter->input(0);
        output = interpreter->output(0);
        const bool validInput =
            input != nullptr && input->type == kTfLiteInt8 &&
            input->dims != nullptr && input->dims->size == 4 &&
            input->dims->data[0] == 1 &&
            input->dims->data[1] == kInputHeight &&
            input->dims->data[2] == kInputWidth &&
            input->dims->data[3] == kInputChannels;
        const bool validOutput =
            output != nullptr && output->type == kTfLiteInt8 &&
            output->bytes >= 2;

        if (!validInput || !validOutput)
        {
            Serial.println(
                "[PersonDetector] Tensores incompatibles con person_detection");
            releaseInterpreter();
            return false;
        }

        ready = true;
        Serial.printf(
            "[PersonDetector] Listo. Entrada=96x96 grayscale, threshold=%.2f\n",
            kPersonThreshold);
        return true;
    }

    bool detectJpeg(const uint8_t *jpegData, size_t jpegLength)
    {
        if (jpegData == nullptr || jpegLength == 0)
        {
            Serial.println("[PersonDetector] JPEG vacio");
            return false;
        }

        if (!ready && !begin())
        {
            Serial.println("[PersonDetector] El detector no pudo inicializarse");
            return false;
        }

        uint16_t width = 0;
        uint16_t height = 0;
        if (!readJpegDimensions(jpegData, jpegLength, width, height))
        {
            Serial.println("[PersonDetector] JPEG invalido o sin dimensiones");
            return false;
        }

        if (static_cast<size_t>(width) > SIZE_MAX / height / 3)
        {
            Serial.println("[PersonDetector] Dimensiones JPEG demasiado grandes");
            return false;
        }

        const size_t rgbLength =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
        uint8_t *rgb = static_cast<uint8_t *>(
            heap_caps_malloc(rgbLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (rgb == nullptr)
        {
            rgb = static_cast<uint8_t *>(
                heap_caps_malloc(rgbLength, MALLOC_CAP_8BIT));
        }
        if (rgb == nullptr)
        {
            Serial.printf(
                "[PersonDetector] Sin memoria para decodificar JPEG %ux%u (%u bytes)\n",
                width, height, static_cast<unsigned>(rgbLength));
            return false;
        }

        const unsigned long decodeStartedAt = millis();
        const bool decoded =
            fmt2rgb888(jpegData, jpegLength, PIXFORMAT_JPEG, rgb);
        const unsigned long decodeMs = millis() - decodeStartedAt;
        if (!decoded)
        {
            Serial.println("[PersonDetector] Error decodificando JPEG");
            heap_caps_free(rgb);
            return false;
        }

        const bool resized = resizeCenterCropToInput(rgb, width, height);
        heap_caps_free(rgb);
        if (!resized)
        {
            Serial.println("[PersonDetector] Error preparando la entrada 96x96");
            return false;
        }

        const unsigned long inferenceStartedAt = millis();
        if (interpreter->Invoke() != kTfLiteOk)
        {
            Serial.println("[PersonDetector] Invoke() fallo");
            return false;
        }
        const unsigned long inferenceMs = millis() - inferenceStartedAt;

        const float personScore =
            dequantize(output->data.int8[kPersonIndex], output);
        const float noPersonScore =
            dequantize(output->data.int8[kNoPersonIndex], output);
        const bool detected = personScore >= kPersonThreshold;

        Serial.printf(
            "[PersonDetector] jpeg=%ux%u decode=%lums inference=%lums "
            "person=%.4f no_person=%.4f threshold=%.2f result=%s\n",
            width, height, decodeMs, inferenceMs, personScore, noPersonScore,
            kPersonThreshold, detected ? "true" : "false");

        return detected;
    }

    bool isReady()
    {
        return ready;
    }
}
