// ============================================================================
// webcam.h - Librería Zero-Copy con Todos los Formatos
// ============================================================================
#ifndef WEBCAM_H
#define WEBCAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#if defined(_WIN32)
  #ifdef BUILDING_DLL
    #define WEBCAM_API __declspec(dllexport)
  #else
    #define WEBCAM_API __declspec(dllimport)
  #endif
#else
  #define WEBCAM_API
#endif

typedef enum {
    WEBCAM_FMT_RGB24  = 0,  // 3 bytes: R, G, B
    WEBCAM_FMT_RGB32  = 1,  // 4 bytes: R, G, B, A
    WEBCAM_FMT_YUYV   = 2,  // 2 bytes: Y0, U, Y1, V
    WEBCAM_FMT_YUV420 = 3,  // 1.5 bytes: Y plane + U plane + V plane
    WEBCAM_FMT_MJPEG  = 4   // Compressed JPEG
} WebcamPixelFormat;

typedef struct Webcam Webcam;

// Zero-copy frame: data points directly to camera's mapped buffer
typedef struct {
    const unsigned char *data;  // Read-only pointer to camera buffer
    int width;
    int height;
    int size;
    WebcamPixelFormat format;
    unsigned long timestamp_ms;
} WebcamFrame;

typedef struct {
    int index;
    char name[128];
    char path[256];
} WebcamInfo;

typedef struct {
    WebcamPixelFormat format;
    int width;
    int height;
    int fps;
} WebcamFormatInfo;

typedef struct {
    WebcamFormatInfo *formats;
    int format_count;
    int max_width;
    int max_height;
    int min_width;
    int min_height;
} WebcamCapabilities;

typedef enum {
    WEBCAM_PARAM_BRIGHTNESS = 1,
    WEBCAM_PARAM_CONTRAST   = 2,
    WEBCAM_PARAM_SATURATION = 3,
    WEBCAM_PARAM_EXPOSURE   = 4,
    WEBCAM_PARAM_FOCUS      = 5,
    WEBCAM_PARAM_ZOOM       = 6,
    WEBCAM_PARAM_GAIN       = 7,
    WEBCAM_PARAM_SHARPNESS  = 8
} WebcamParameter;

// Device enumeration
WEBCAM_API WebcamInfo* webcam_list_devices(int *count);
WEBCAM_API void webcam_free_list(WebcamInfo *list);

// Capability query
WEBCAM_API WebcamCapabilities* webcam_query_capabilities(int device_index);
WEBCAM_API void webcam_free_capabilities(WebcamCapabilities *caps);
WEBCAM_API WebcamFormatInfo* webcam_find_best_format(
    WebcamCapabilities *caps,
    int preferred_width,
    int preferred_height,
    WebcamPixelFormat preferred_format
);

// Camera lifecycle (ZERO-COPY ONLY)
WEBCAM_API Webcam* webcam_open(int width, int height, int device_index, 
                               WebcamPixelFormat format);
WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame);
WEBCAM_API void webcam_release_frame(Webcam *cam);
WEBCAM_API void webcam_close(Webcam *cam);

// Information
WEBCAM_API int webcam_get_actual_width(Webcam *cam);
WEBCAM_API int webcam_get_actual_height(Webcam *cam);
WEBCAM_API WebcamPixelFormat webcam_get_format(Webcam *cam);

// Controls
WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param);
WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value);
WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto);

#ifdef __cplusplus
}
#endif

#endif // WEBCAM_H
