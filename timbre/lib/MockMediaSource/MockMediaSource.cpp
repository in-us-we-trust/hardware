#include "MockMediaSource.h"
#include "mock_frame.h" // JPEG embebido: mock_frame_jpg / mock_frame_jpg_len

bool MockMediaSource::begin()
{
    Serial.printf("[MOCK] fuente lista: JPEG embebido de %u bytes @ %d fps\n",
                  (unsigned)mock_frame_jpg_len, _fps);
    return true;
}

bool MockMediaSource::nextVideoFrame(const uint8_t **data, size_t *len)
{
    // Siempre el mismo frame en loop. Para simular movimiento, aca se podrian
    // alternar 2-3 JPEGs embebidos.
    *data = mock_frame_jpg;
    *len = mock_frame_jpg_len;
    return true;
}
