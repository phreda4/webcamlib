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

// --- TIPOS ---

typedef enum {
    WEBCAM_FMT_RGB24 = 0, // R, G, B (3 bytes)
    WEBCAM_FMT_YUYV  = 1  // Y, U, Y, V (2 bytes)
} WebcamPixelFormat;

typedef struct Webcam Webcam;

typedef struct {
    unsigned char *data;
    int width;
    int height;
    int size;
    WebcamPixelFormat format;
    unsigned long timestamp_ms; 
} WebcamFrame;

typedef struct {
    int index;
    char name[128];
    char path[256]; // Util para identificacion unica
} WebcamInfo;

// Parametros de control
typedef enum {
    WEBCAM_PARAM_BRIGHTNESS = 1,
    WEBCAM_PARAM_CONTRAST   = 2,
    WEBCAM_PARAM_SATURATION = 3,
    WEBCAM_PARAM_EXPOSURE   = 4,
    WEBCAM_PARAM_FOCUS      = 5,
    WEBCAM_PARAM_ZOOM       = 6
} WebcamParameter;

// Callback de logs para debug
typedef void (*WebcamLogFunc)(const char* msg);

// --- API ---

// Configuracion global
WEBCAM_API void webcam_set_logger(WebcamLogFunc func);

// Listado
WEBCAM_API WebcamInfo* webcam_list_devices(int *count);
WEBCAM_API void webcam_free_list(WebcamInfo *list);

// Ciclo de vida
// width/height: Solicitado. La camara puede dar uno cercano si no soporta el exacto.
WEBCAM_API Webcam* webcam_open(int width, int height, int device_index, WebcamPixelFormat format);
WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame);
WEBCAM_API void webcam_close(Webcam *cam);

// Getters informativos
WEBCAM_API int webcam_get_actual_width(Webcam *cam);
WEBCAM_API int webcam_get_actual_height(Webcam *cam);

// Controles
WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param);
WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value);
WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto);

#ifdef __cplusplus
}
#endif

#endif // WEBCAM_H