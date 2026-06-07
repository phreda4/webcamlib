// ============================================================================
// webcam_common.c
// ============================================================================
#include "webcam.h"
#include <stdlib.h>

WEBCAM_API void webcam_free_list(WebcamInfo *list) {
    if (list) free(list);
}

WEBCAM_API void webcam_free_capabilities(WebcamCapabilities *caps) {
    if (caps) {
        if (caps->formats) free(caps->formats);
        free(caps);
    }
}

/*
 * webcam_calc_frame_size
 * ----------------------
 * Helper compartido por las implementaciones de plataforma.
 * Para formatos de tamaño fijo calcula desde dimensiones;
 * para MJPEG usa bytesused (el driver ya sabe el tamaño real).
 */
WEBCAM_API size_t webcam_calc_frame_size(WebcamPixelFormat format,
                                          int width, int height,
                                          size_t bytesused) {
    size_t pixels = (size_t)width * height;
    switch (format) {
        case WEBCAM_FMT_RGB24:  return pixels * 3;
        case WEBCAM_FMT_RGB32:  return pixels * 4;
        case WEBCAM_FMT_YUYV:   return pixels * 2;
        case WEBCAM_FMT_YUV420: return pixels * 3 / 2;
        case WEBCAM_FMT_MJPEG:  return bytesused;
        default:                return bytesused;
    }
}

WEBCAM_API WebcamFormatInfo* webcam_find_best_format(
    WebcamCapabilities *caps,
    int preferred_width,
    int preferred_height,
    WebcamPixelFormat preferred_format) {

    if (!caps || !caps->formats || caps->format_count == 0) return NULL;

    WebcamFormatInfo *best = NULL;
    int best_score = -999999;

    for (int i = 0; i < caps->format_count; i++) {
        WebcamFormatInfo *fmt = &caps->formats[i];
        int score = 0;

        /* Fuerte preferencia por el formato exacto */
        if (fmt->format == preferred_format) score += 10000;

        /* Penalizar diferencia de resolución */
        int width_diff  = abs(fmt->width  - preferred_width);
        int height_diff = abs(fmt->height - preferred_height);
        score -= (width_diff + height_diff);

        /* Leve preferencia por 30 fps cuando el resto puntúa igual */
        score -= abs(fmt->fps - 30) * 3;

        if (score > best_score) {
            best_score = score;
            best = fmt;
        }
    }

    return best;
}
