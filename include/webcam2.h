#ifndef WEBCAM_H
#define WEBCAM_H

#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
  #ifdef BUILDING_DLL
    #define WEBCAM_API __declspec(dllexport)
  #else
    #define WEBCAM_API __declspec(dllimport)
  #endif
#else
  #define WEBCAM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Formatos de pixel
typedef enum {
    WEBCAM_FMT_RGB24   = 0, // 3 bytes: R, G, B
    WEBCAM_FMT_RGB32   = 1, // 4 bytes: B, G, R, A (o similar)
    WEBCAM_FMT_YUYV    = 2, // 2 bytes: Y, U, Y, V (Packed)
    WEBCAM_FMT_YUV420P = 3  // 1.5 bytes: Yyyy... Uu... Vv... (Planar - I420)
} WebcamPixelFormat;

// Parámetros de control unificados
typedef enum {
    WEBCAM_PARAM_BRIGHTNESS = 1,
    WEBCAM_PARAM_CONTRAST   = 2,
    WEBCAM_PARAM_SATURATION = 3,
    WEBCAM_PARAM_EXPOSURE   = 4,
    WEBCAM_PARAM_FOCUS      = 5,
    WEBCAM_PARAM_ZOOM       = 6,
    WEBCAM_PARAM_GAIN       = 7
} WebcamParameter;

typedef struct Webcam Webcam;

typedef struct {
    unsigned char *data;        // Puntero directo a los datos (Zero Copy)
    int width;
    int height;
    int stride;                 // Bytes por linea (importante en Zero Copy)
    int size;                   // Tamaño total en bytes
    WebcamPixelFormat format;
    unsigned long timestamp_ms;
    
    // Espacio reservado para uso interno del driver (sin #ifdefs públicos)
    // Linux usa private_data[0] como índice (int cast).
    // Windows usa private_data[0] como puntero a IMediaBuffer.
    void *private_data[2]; 
} WebcamFrame;

typedef struct {
    int index;
    char name[128];
    char path[256];
} WebcamInfo;

// --- API ---

WEBCAM_API WebcamInfo* webcam_list_devices(int *count);
WEBCAM_API void webcam_free_list(WebcamInfo *list);

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index, WebcamPixelFormat format);
WEBCAM_API void webcam_close(Webcam *cam);

// Zero Copy: Captura el puntero del driver
WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame);

// Zero Copy: Libera el puntero al driver
WEBCAM_API void webcam_release_frame(Webcam *cam, WebcamFrame *frame);

// Controles
WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param);
WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value);
WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto);

// Getters informativos
WEBCAM_API int webcam_get_actual_width(Webcam *cam);
WEBCAM_API int webcam_get_actual_height(Webcam *cam);

#ifdef __cplusplus
}
#endif

#endif // WEBCAM_H