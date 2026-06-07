// ============================================================================
// webcam_win.cpp - Windows Implementation with Zero-Copy
// ============================================================================
#ifdef _WIN32

#include "webcam.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")

template <class T> static void SafeRelease(T **ppT) {
    if (*ppT) { (*ppT)->Release(); *ppT = NULL; }
}

struct Webcam {
    IMFSourceReader  *reader;
    IAMVideoProcAmp  *procAmp;
    IAMCameraControl *camControl;
    int               actual_width;
    int               actual_height;
    WebcamPixelFormat format;
    IMFSample        *current_sample;
    IMFMediaBuffer   *current_buffer;
};

extern "C" {

/* Definido en webcam_common.c */
extern size_t webcam_calc_frame_size(WebcamPixelFormat format,
                                      int width, int height,
                                      size_t bytesused);

/* ── Helpers internos ───────────────────────────────────────────────────── */

static void wchar_to_char(const WCHAR *src, char *dst, int max_len) {
    if (!src || !dst) return;
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, max_len, NULL, NULL);
}

/*
 * webcam_format_to_mf_guid
 * ------------------------
 * Elimina los switch duplicados que había en webcam_open original.
 */
static GUID webcam_format_to_mf_guid(WebcamPixelFormat format) {
    switch (format) {
        case WEBCAM_FMT_RGB24:  return MFVideoFormat_RGB24;
        case WEBCAM_FMT_RGB32:  return MFVideoFormat_RGB32;
        case WEBCAM_FMT_YUYV:   return MFVideoFormat_YUY2;
        case WEBCAM_FMT_YUV420: return MFVideoFormat_I420;
        case WEBCAM_FMT_MJPEG:  return MFVideoFormat_MJPG;
        default:                return MFVideoFormat_YUY2;
    }
}

/*
 * mf_guid_to_webcam_format
 * ------------------------
 * Convierte un GUID de subtype MF a WebcamPixelFormat.
 * Devuelve 0 si no es reconocido.
 */
static int mf_guid_to_webcam_format(const GUID &subtype, WebcamPixelFormat *out) {
    if      (IsEqualGUID(subtype, MFVideoFormat_RGB24)) { *out = WEBCAM_FMT_RGB24;  return 1; }
    else if (IsEqualGUID(subtype, MFVideoFormat_RGB32)) { *out = WEBCAM_FMT_RGB32;  return 1; }
    else if (IsEqualGUID(subtype, MFVideoFormat_YUY2))  { *out = WEBCAM_FMT_YUYV;   return 1; }
    else if (IsEqualGUID(subtype, MFVideoFormat_I420))  { *out = WEBCAM_FMT_YUV420; return 1; }
    else if (IsEqualGUID(subtype, MFVideoFormat_MJPG))  { *out = WEBCAM_FMT_MJPEG;  return 1; }
    return 0;
}

/*
 * enum_devices_internal
 * ---------------------
 * Encapsula la enumeración repetida de devices MF.
 * El caller es responsable de liberar ppDevices con CoTaskMemFree
 * y cada elemento con SafeRelease.
 */
static HRESULT enum_devices_internal(IMFActivate ***ppDevices, UINT32 *count) {
    IMFAttributes *pConfig = NULL;
    HRESULT hr = MFCreateAttributes(&pConfig, 1);
    if (FAILED(hr)) return hr;

    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    hr = MFEnumDeviceSources(pConfig, ppDevices, count);
    pConfig->Release();
    return hr;
}

/* ── API ────────────────────────────────────────────────────────────────── */

WEBCAM_API WebcamInfo* webcam_list_devices(int *count) {
    *count = 0;

    /* MFStartup/MFShutdown balanceados dentro de esta función */
    if (FAILED(MFStartup(MF_VERSION))) return NULL;

    IMFActivate **ppDevices = NULL;
    UINT32 dev_count = 0;

    if (FAILED(enum_devices_internal(&ppDevices, &dev_count)) || dev_count == 0) {
        CoTaskMemFree(ppDevices);
        MFShutdown();
        return NULL;
    }

    WebcamInfo *list = (WebcamInfo*)calloc(dev_count, sizeof(WebcamInfo));
    if (!list) {
        for (UINT32 i = 0; i < dev_count; i++) SafeRelease(&ppDevices[i]);
        CoTaskMemFree(ppDevices);
        MFShutdown();
        return NULL;
    }

    *count = (int)dev_count;

    for (UINT32 i = 0; i < dev_count; i++) {
        list[i].index = i;
        WCHAR *name = NULL, *sym = NULL;

        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                          &name, NULL);
        ppDevices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym, NULL);

        if (name) { wchar_to_char(name, list[i].name, 128); CoTaskMemFree(name); }
        if (sym)  { wchar_to_char(sym,  list[i].path, 256); CoTaskMemFree(sym);  }

        SafeRelease(&ppDevices[i]);
    }
    CoTaskMemFree(ppDevices);

    MFShutdown();
    return list;
}

WEBCAM_API WebcamCapabilities* webcam_query_capabilities(int device_index) {
    if (FAILED(MFStartup(MF_VERSION))) return NULL;

    IMFActivate **ppDevices = NULL;
    UINT32 count = 0;

    if (FAILED(enum_devices_internal(&ppDevices, &count)) ||
        device_index >= (int)count) {
        for (UINT32 i = 0; i < count; i++) SafeRelease(&ppDevices[i]);
        CoTaskMemFree(ppDevices);
        MFShutdown();
        return NULL;
    }

    IMFMediaSource *pSource = NULL;
    HRESULT hr = ppDevices[device_index]->ActivateObject(IID_PPV_ARGS(&pSource));
    for (UINT32 i = 0; i < count; i++) SafeRelease(&ppDevices[i]);
    CoTaskMemFree(ppDevices);

    if (FAILED(hr)) { MFShutdown(); return NULL; }

    IMFSourceReader *pReader = NULL;
    hr = MFCreateSourceReaderFromMediaSource(pSource, NULL, &pReader);
    SafeRelease(&pSource);
    if (FAILED(hr)) { MFShutdown(); return NULL; }

    /* BUG FIX: null checks después de malloc */
    WebcamCapabilities *caps = (WebcamCapabilities*)calloc(1, sizeof(WebcamCapabilities));
    if (!caps) { SafeRelease(&pReader); MFShutdown(); return NULL; }

    WebcamFormatInfo *formats = (WebcamFormatInfo*)malloc(100 * sizeof(WebcamFormatInfo));
    if (!formats) { free(caps); SafeRelease(&pReader); MFShutdown(); return NULL; }

    int format_count = 0;
    caps->min_width  = 99999;
    caps->min_height = 99999;

    for (DWORD dwIndex = 0; ; dwIndex++) {
        IMFMediaType *pType = NULL;
        if (FAILED(pReader->GetNativeMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, dwIndex, &pType))) break;

        GUID subtype;
        UINT32 w = 0, h = 0;

        if (SUCCEEDED(pType->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            SUCCEEDED(MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &w, &h)) &&
            format_count < 100) {

            WebcamPixelFormat fmt;
            if (mf_guid_to_webcam_format(subtype, &fmt)) {
                /* FPS real desde el media type */
                UINT32 fps_num = 0, fps_den = 1;
                MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, &fps_num, &fps_den);
                int fps = (fps_den > 0 && fps_num > 0) ? (int)(fps_num / fps_den) : 30;

                formats[format_count].format = fmt;
                formats[format_count].width  = w;
                formats[format_count].height = h;
                formats[format_count].fps    = fps;

                if (w > (UINT32)caps->max_width)  caps->max_width  = w;
                if (h > (UINT32)caps->max_height) caps->max_height = h;
                if (w < (UINT32)caps->min_width)  caps->min_width  = w;
                if (h < (UINT32)caps->min_height) caps->min_height = h;

                format_count++;
            }
        }
        SafeRelease(&pType);
    }

    SafeRelease(&pReader);
    MFShutdown();

    /* BUG FIX: liberar formats si no se encontró nada (era memory leak) */
    if (format_count == 0) {
        free(formats);
        free(caps);
        return NULL;
    }

    caps->formats      = (WebcamFormatInfo*)realloc(formats,
                            format_count * sizeof(WebcamFormatInfo));
    caps->format_count = format_count;
    return caps;
}

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index,
                               WebcamPixelFormat format) {
    /* MFStartup aquí; MFShutdown en webcam_close */
    if (FAILED(MFStartup(MF_VERSION))) return NULL;

    IMFActivate **ppDevices = NULL;
    UINT32 count = 0;

    if (FAILED(enum_devices_internal(&ppDevices, &count)) ||
        device_index >= (int)count) {
        for (UINT32 i = 0; i < count; i++) SafeRelease(&ppDevices[i]);
        CoTaskMemFree(ppDevices);
        MFShutdown();
        return NULL;
    }

    IMFMediaSource *pSource = NULL;
    HRESULT hr = ppDevices[device_index]->ActivateObject(IID_PPV_ARGS(&pSource));
    for (UINT32 i = 0; i < count; i++) SafeRelease(&ppDevices[i]);
    CoTaskMemFree(ppDevices);
    if (FAILED(hr)) { MFShutdown(); return NULL; }

    IMFSourceReader *pReader = NULL;
    hr = MFCreateSourceReaderFromMediaSource(pSource, NULL, &pReader);
    if (FAILED(hr)) {
        SafeRelease(&pSource);
        MFShutdown();
        return NULL;
    }

    /*
     * Configurar tipo de media.
     * FIX: helper webcam_format_to_mf_guid elimina el switch duplicado
     * que aparecía dos veces en el original.
     */
    IMFMediaType *pType = NULL;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pType->SetGUID(MF_MT_SUBTYPE, webcam_format_to_mf_guid(format));
    MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);

    hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                       NULL, pType);

    if (FAILED(hr)) {
        /* Fallback: intentar sin resolución específica */
        pType->DeleteAllItems();
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pType->SetGUID(MF_MT_SUBTYPE, webcam_format_to_mf_guid(format));
        hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                           NULL, pType);
    }
    SafeRelease(&pType);

    if (FAILED(hr)) {
        SafeRelease(&pReader);
        SafeRelease(&pSource);
        MFShutdown();
        return NULL;
    }

    /* Leer resolución real negociada */
    int final_w = width, final_h = height;
    IMFMediaType *pCurrentType = NULL;
    if (SUCCEEDED(pReader->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType))) {
        MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE,
                           (UINT32*)&final_w, (UINT32*)&final_h);
        SafeRelease(&pCurrentType);
    }

    Webcam *cam = new Webcam();
    cam->reader         = pReader;
    cam->actual_width   = final_w;
    cam->actual_height  = final_h;
    cam->format         = format;
    cam->current_sample = NULL;
    cam->current_buffer = NULL;

    pSource->QueryInterface(IID_PPV_ARGS(&cam->procAmp));
    pSource->QueryInterface(IID_PPV_ARGS(&cam->camControl));
    SafeRelease(&pSource);

    return cam;
}

WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame) {
    if (!cam || !cam->reader || !frame) return -1;

    /* Liberar sample/buffer anterior */
    SafeRelease(&cam->current_buffer);
    SafeRelease(&cam->current_sample);

    DWORD flags = 0;
    HRESULT hr = cam->reader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0, NULL, &flags, NULL, &cam->current_sample);

    if (FAILED(hr) || !cam->current_sample) return -1;

    /*
     * Optimización zero-copy: si el sample tiene un único buffer,
     * accedemos directo sin ConvertToContiguousBuffer (que puede copiar).
     * Solo consolidamos cuando hay múltiples buffers (raro en webcams).
     */
    DWORD buf_count = 0;
    cam->current_sample->GetBufferCount(&buf_count);

    IMFMediaBuffer *pBuf = NULL;
    if (buf_count == 1) {
        cam->current_sample->GetBufferByIndex(0, &pBuf);
    } else {
        cam->current_sample->ConvertToContiguousBuffer(&pBuf);
    }

    if (!pBuf) return -1;
    cam->current_buffer = pBuf;

    BYTE  *pData = NULL;
    DWORD  len   = 0;
    if (FAILED(cam->current_buffer->Lock(&pData, NULL, &len))) {
        SafeRelease(&cam->current_buffer);
        return -1;
    }

    frame->data         = (const unsigned char*)pData;
    frame->width        = cam->actual_width;
    frame->height       = cam->actual_height;
    frame->format       = cam->format;
    frame->timestamp_ms = GetTickCount64();
    frame->size         = webcam_calc_frame_size(cam->format,
                              cam->actual_width, cam->actual_height, len);
    return 0;
}

WEBCAM_API void webcam_release_frame(Webcam *cam) {
    if (!cam || !cam->current_buffer) return;

    cam->current_buffer->Unlock();

    /*
     * BUG FIX: SafeRelease después de Unlock.
     * Sin esto, webcam_close llamaba Unlock() una segunda vez
     * sobre el mismo buffer → comportamiento indefinido.
     */
    SafeRelease(&cam->current_buffer);
}

WEBCAM_API void webcam_close(Webcam *cam) {
    if (!cam) return;

    /* current_buffer puede estar locked si el usuario olvidó release_frame */
    if (cam->current_buffer) {
        cam->current_buffer->Unlock();
    }
    SafeRelease(&cam->current_buffer);
    SafeRelease(&cam->current_sample);
    SafeRelease(&cam->procAmp);
    SafeRelease(&cam->camControl);
    SafeRelease(&cam->reader);

    /* Balancear el MFStartup de webcam_open */
    MFShutdown();

    delete cam;
}

WEBCAM_API int webcam_get_actual_width(Webcam *cam) {
    return cam ? cam->actual_width : 0;
}

WEBCAM_API int webcam_get_actual_height(Webcam *cam) {
    return cam ? cam->actual_height : 0;
}

WEBCAM_API WebcamPixelFormat webcam_get_format(Webcam *cam) {
    return cam ? cam->format : WEBCAM_FMT_YUYV;
}

/* ── Helpers internos de parámetros ─────────────────────────────────────── */

static long get_proc_amp(Webcam *cam, long prop) {
    if (!cam->procAmp) return -1;
    long val, f;
    return SUCCEEDED(cam->procAmp->Get(prop, &val, &f)) ? val : -1;
}

static long get_cam_ctrl(Webcam *cam, long prop) {
    if (!cam->camControl) return -1;
    long val, f;
    return SUCCEEDED(cam->camControl->Get(prop, &val, &f)) ? val : -1;
}

static int set_proc_amp(Webcam *cam, long prop, long val, int is_auto) {
    if (!cam->procAmp) return -1;
    return SUCCEEDED(cam->procAmp->Set(prop, val,
        is_auto ? VideoProcAmp_Flags_Auto : VideoProcAmp_Flags_Manual)) ? 0 : -1;
}

static int set_cam_ctrl(Webcam *cam, long prop, long val, int is_auto) {
    if (!cam->camControl) return -1;
    return SUCCEEDED(cam->camControl->Set(prop, val,
        is_auto ? CameraControl_Flags_Auto : CameraControl_Flags_Manual)) ? 0 : -1;
}

WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param) {
    if (!cam) return -1;
    switch (param) {
        case WEBCAM_PARAM_BRIGHTNESS: return get_proc_amp(cam, VideoProcAmp_Brightness);
        case WEBCAM_PARAM_CONTRAST:   return get_proc_amp(cam, VideoProcAmp_Contrast);
        case WEBCAM_PARAM_SATURATION: return get_proc_amp(cam, VideoProcAmp_Saturation);
        case WEBCAM_PARAM_SHARPNESS:  return get_proc_amp(cam, VideoProcAmp_Sharpness);
        case WEBCAM_PARAM_GAIN:       return get_proc_amp(cam, VideoProcAmp_Gain);
        case WEBCAM_PARAM_EXPOSURE:   return get_cam_ctrl(cam, CameraControl_Exposure);
        case WEBCAM_PARAM_FOCUS:      return get_cam_ctrl(cam, CameraControl_Focus);
        case WEBCAM_PARAM_ZOOM:       return get_cam_ctrl(cam, CameraControl_Zoom);
    }
    return -1;
}

WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value) {
    if (!cam) return -1;
    switch (param) {
        case WEBCAM_PARAM_BRIGHTNESS: return set_proc_amp(cam, VideoProcAmp_Brightness, value, 0);
        case WEBCAM_PARAM_CONTRAST:   return set_proc_amp(cam, VideoProcAmp_Contrast,   value, 0);
        case WEBCAM_PARAM_SATURATION: return set_proc_amp(cam, VideoProcAmp_Saturation, value, 0);
        case WEBCAM_PARAM_SHARPNESS:  return set_proc_amp(cam, VideoProcAmp_Sharpness,  value, 0);
        case WEBCAM_PARAM_GAIN:       return set_proc_amp(cam, VideoProcAmp_Gain,        value, 0);
        case WEBCAM_PARAM_EXPOSURE:   return set_cam_ctrl(cam, CameraControl_Exposure,  value, 0);
        case WEBCAM_PARAM_FOCUS:      return set_cam_ctrl(cam, CameraControl_Focus,     value, 0);
        case WEBCAM_PARAM_ZOOM:       return set_cam_ctrl(cam, CameraControl_Zoom,      value, 0);
    }
    return -1;
}

WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto) {
    if (!cam) return -1;
    switch (param) {
        case WEBCAM_PARAM_EXPOSURE: return set_cam_ctrl(cam, CameraControl_Exposure, 0, is_auto);
        case WEBCAM_PARAM_FOCUS:    return set_cam_ctrl(cam, CameraControl_Focus,    0, is_auto);
    }
    return -1;
}

} /* extern "C" */

#endif /* _WIN32 */
