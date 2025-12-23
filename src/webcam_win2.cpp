#include "webcam.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

template <class T> void SafeRelease(T **ppT) {
    if (*ppT) { (*ppT)->Release(); *ppT = NULL; }
}

struct Webcam {
    IMFSourceReader *reader;
    IAMVideoProcAmp *procAmp;
    IAMCameraControl *camControl;
    int actual_width;
    int actual_height;
    int stride; // stride puede ser negativo en Windows (bottom-up)
    WebcamPixelFormat format;
};

extern "C" {

void wchar_to_char(const WCHAR *src, char *dst, int max_len) {
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, max_len, NULL, NULL);
}

WEBCAM_API WebcamInfo* webcam_list_devices(int *count) {
    // ... (Mismo código de listado que la versión anterior) ...
    *count = 0;
    MFStartup(MF_VERSION);
    IMFAttributes *pConfig = NULL;
    IMFActivate **ppDevices = NULL;
    UINT32 dev_count = 0;

    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(MFEnumDeviceSources(pConfig, &ppDevices, &dev_count))) return NULL;
    pConfig->Release();

    if (dev_count == 0) return NULL;
    WebcamInfo *list = (WebcamInfo*)calloc(dev_count, sizeof(WebcamInfo));
    *count = (int)dev_count;

    for (UINT32 i = 0; i < dev_count; i++) {
        list[i].index = i;
        WCHAR *name = NULL, *sym = NULL;
        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, NULL);
        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym, NULL);
        if (name) { wchar_to_char(name, list[i].name, 128); CoTaskMemFree(name); }
        if (sym)  { wchar_to_char(sym, list[i].path, 256); CoTaskMemFree(sym); }
        SafeRelease(&ppDevices[i]);
    }
    CoTaskMemFree(ppDevices);
    return list;
}

WEBCAM_API void webcam_free_list(WebcamInfo *list) { free(list); }

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index, WebcamPixelFormat format) {
    MFStartup(MF_VERSION);
    // ... (Inicialización source similar) ...
    IMFAttributes *pConfig = NULL;
    IMFActivate **ppDevices = NULL;
    UINT32 count = 0;
    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    MFEnumDeviceSources(pConfig, &ppDevices, &count);
    pConfig->Release();
    if (device_index >= count) return NULL;
    IMFMediaSource *pSource = NULL;
    ppDevices[device_index]->ActivateObject(IID_PPV_ARGS(&pSource));
    for(UINT32 i=0; i<count; i++) SafeRelease(&ppDevices[i]);
    CoTaskMemFree(ppDevices);
    if (!pSource) return NULL;

    IMFAttributes *pReaderConfig = NULL;
    MFCreateAttributes(&pReaderConfig, 1);
    pReaderConfig->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1);
    
    IMFSourceReader *pReader = NULL;
    if (FAILED(MFCreateSourceReaderFromMediaSource(pSource, pReaderConfig, &pReader))) {
        SafeRelease(&pSource); return NULL;
    }
    SafeRelease(&pReaderConfig);

    // Selección de formato
    IMFMediaType *pType = NULL;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    
    GUID subtype = MFVideoFormat_RGB24;
    switch(format) {
        case WEBCAM_FMT_YUYV:    subtype = MFVideoFormat_YUY2; break;
        case WEBCAM_FMT_RGB32:   subtype = MFVideoFormat_RGB32; break;
        case WEBCAM_FMT_YUV420P: subtype = MFVideoFormat_I420; break;
        default:                 subtype = MFVideoFormat_RGB24; break;
    }
    pType->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
    
    if (FAILED(pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType))) {
        // Fallback genérico
        pType->DeleteAllItems();
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pType->SetGUID(MF_MT_SUBTYPE, subtype);
        pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
    }
    SafeRelease(&pType);

    // Leer dimensiones reales y stride
    int real_w = width, real_h = height;
    LONG lStride = 0;
    IMFMediaType *curr = NULL;
    if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &curr))) {
        MFGetAttributeSize(curr, MF_MT_FRAME_SIZE, (UINT32*)&real_w, (UINT32*)&real_h);
        curr->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&lStride);
        SafeRelease(&curr);
    }

    Webcam *cam = new Webcam();
    cam->reader = pReader;
    cam->actual_width = real_w;
    cam->actual_height = real_h;
    cam->stride = (int)lStride;
    cam->format = format;
    
    pSource->QueryInterface(IID_PPV_ARGS(&cam->procAmp));
    pSource->QueryInterface(IID_PPV_ARGS(&cam->camControl));
    SafeRelease(&pSource);

    return cam;
}

WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame) {
    if (!cam || !cam->reader) return -1;
    
    IMFSample *pSample = NULL;
    DWORD flags = 0;
    HRESULT hr = cam->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, NULL, &pSample);
    
    if (FAILED(hr) || !pSample) return -1;

    IMFMediaBuffer *pBuffer = NULL;
    if (FAILED(pSample->ConvertToContiguousBuffer(&pBuffer))) {
        SafeRelease(&pSample); return -1;
    }

    BYTE *pData = NULL;
    DWORD len = 0;
    if (FAILED(pBuffer->Lock(&pData, NULL, &len))) {
        SafeRelease(&pBuffer); SafeRelease(&pSample); return -1;
    }

    frame->data = pData;
    frame->size = (int)len;
    frame->width = cam->actual_width;
    frame->height = cam->actual_height;
    frame->stride = cam->stride; // Puede ser negativo (imagen invertida verticalmente en Windows a veces)
    frame->format = cam->format;
    frame->timestamp_ms = GetTickCount();
    
    // Guardar puntero para liberación
    frame->private_data[0] = pBuffer; 
    
    SafeRelease(&pSample);
    return 0;
}

WEBCAM_API void webcam_release_frame(Webcam *cam, WebcamFrame *frame) {
    IMFMediaBuffer *pBuffer = (IMFMediaBuffer*)frame->private_data[0];
    if (pBuffer) {
        pBuffer->Unlock();
        pBuffer->Release();
        frame->private_data[0] = NULL;
    }
}

// ... (Resto de funciones: webcam_close, controls, igual que en la versión anterior) ...
WEBCAM_API void webcam_close(Webcam *cam) {
    if (cam) {
        SafeRelease(&cam->procAmp);
        SafeRelease(&cam->camControl);
        SafeRelease(&cam->reader);
        delete cam;
    }
}
WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param) {
    long val, f;
    if (cam->procAmp) {
        if (param == WEBCAM_PARAM_BRIGHTNESS) { cam->procAmp->Get(VideoProcAmp_Brightness, &val, &f); return val; }
        if (param == WEBCAM_PARAM_CONTRAST)   { cam->procAmp->Get(VideoProcAmp_Contrast, &val, &f); return val; }
        if (param == WEBCAM_PARAM_SATURATION) { cam->procAmp->Get(VideoProcAmp_Saturation, &val, &f); return val; }
        if (param == WEBCAM_PARAM_GAIN)       { cam->procAmp->Get(VideoProcAmp_Gain, &val, &f); return val; }
    }
    if (cam->camControl) {
        if (param == WEBCAM_PARAM_EXPOSURE) { cam->camControl->Get(CameraControl_Exposure, &val, &f); return val; }
        if (param == WEBCAM_PARAM_FOCUS)    { cam->camControl->Get(CameraControl_Focus, &val, &f); return val; }
        if (param == WEBCAM_PARAM_ZOOM)     { cam->camControl->Get(CameraControl_Zoom, &val, &f); return val; }
    }
    return -1;
}

WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value) {
    if (cam->procAmp) {
        if (param == WEBCAM_PARAM_BRIGHTNESS) return SUCCEEDED(cam->procAmp->Set(VideoProcAmp_Brightness, value, 2)) ? 0 : -1;
        if (param == WEBCAM_PARAM_CONTRAST)   return SUCCEEDED(cam->procAmp->Set(VideoProcAmp_Contrast, value, 2)) ? 0 : -1;
        if (param == WEBCAM_PARAM_SATURATION) return SUCCEEDED(cam->procAmp->Set(VideoProcAmp_Saturation, value, 2)) ? 0 : -1;
        if (param == WEBCAM_PARAM_GAIN)       return SUCCEEDED(cam->procAmp->Set(VideoProcAmp_Gain, value, 2)) ? 0 : -1;
    }
    if (cam->camControl) {
        if (param == WEBCAM_PARAM_EXPOSURE) return SUCCEEDED(cam->camControl->Set(CameraControl_Exposure, value, 2)) ? 0 : -1;
        if (param == WEBCAM_PARAM_FOCUS)    return SUCCEEDED(cam->camControl->Set(CameraControl_Focus, value, 2)) ? 0 : -1;
        if (param == WEBCAM_PARAM_ZOOM)     return SUCCEEDED(cam->camControl->Set(CameraControl_Zoom, value, 2)) ? 0 : -1;
    }
    return -1;
}

WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto) {
    long flag = is_auto ? 1 : 2; 
    if (param == WEBCAM_PARAM_EXPOSURE && cam->camControl) {
        long val, f; cam->camControl->Get(CameraControl_Exposure, &val, &f);
        return SUCCEEDED(cam->camControl->Set(CameraControl_Exposure, val, flag)) ? 0 : -1;
    }
    if (param == WEBCAM_PARAM_FOCUS && cam->camControl) {
        long val, f; cam->camControl->Get(CameraControl_Focus, &val, &f);
        return SUCCEEDED(cam->camControl->Set(CameraControl_Focus, val, flag)) ? 0 : -1;
    }
    return -1;
}

WEBCAM_API int webcam_get_actual_width(Webcam *cam) { return cam ? cam->actual_width : 0; }
WEBCAM_API int webcam_get_actual_height(Webcam *cam) { return cam ? cam->actual_height : 0; }

} // extern C