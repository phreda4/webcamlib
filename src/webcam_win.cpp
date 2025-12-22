#include "webcam.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <strmif.h> 

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "shlwapi.lib")

extern "C" void webcam_log(const char *format, ...);

template <class T> void SafeRelease(T **ppT) {
    if (*ppT) { (*ppT)->Release(); *ppT = NULL; }
}

struct Webcam {
    IMFSourceReader *reader;
    IAMVideoProcAmp *procAmp;
    IAMCameraControl *camControl;
    int actual_width;
    int actual_height;
    WebcamPixelFormat format;
};

extern "C" {

void wchar_to_char(const WCHAR *src, char *dst, int max_len) {
    if(!src || !dst) return;
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, max_len, NULL, NULL);
}

WEBCAM_API WebcamInfo* webcam_list_devices(int *count) {
    *count = 0;
    if (FAILED(MFStartup(MF_VERSION))) return NULL;

    IMFAttributes *pConfig = NULL;
    IMFActivate **ppDevices = NULL;
    UINT32 dev_count = 0;

    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    
    if (FAILED(MFEnumDeviceSources(pConfig, &ppDevices, &dev_count))) {
        pConfig->Release();
        return NULL;
    }
    pConfig->Release();

    if (dev_count == 0) return NULL;

    WebcamInfo *list = (WebcamInfo*)calloc(dev_count, sizeof(WebcamInfo));
    *count = (int)dev_count;

    for (UINT32 i = 0; i < dev_count; i++) {
        list[i].index = i;
        WCHAR *name = NULL;
        WCHAR *sym = NULL;
        
        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, NULL);
        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym, NULL);
        
        if (name) { wchar_to_char(name, list[i].name, 128); CoTaskMemFree(name); }
        if (sym)  { wchar_to_char(sym, list[i].path, 256); CoTaskMemFree(sym); }
        
        SafeRelease(&ppDevices[i]);
    }
    CoTaskMemFree(ppDevices);
    return list;
}

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index, WebcamPixelFormat format) {
    HRESULT hr = MFStartup(MF_VERSION);
    
    IMFAttributes *pConfig = NULL;
    IMFActivate **ppDevices = NULL;
    UINT32 count = 0;
    
    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    MFEnumDeviceSources(pConfig, &ppDevices, &count);
    pConfig->Release();

    if (device_index >= count) {
        webcam_log("Indice de dispositivo fuera de rango.");
        CoTaskMemFree(ppDevices);
        return NULL;
    }

    IMFMediaSource *pSource = NULL;
    hr = ppDevices[device_index]->ActivateObject(IID_PPV_ARGS(&pSource));
    
    for(UINT32 i=0; i<count; i++) SafeRelease(&ppDevices[i]);
    CoTaskMemFree(ppDevices);

    if (FAILED(hr)) return NULL;

    // Crear Source Reader con procesado de video habilitado (para decodificar MJPEG si es necesario)
    IMFAttributes *pReaderConfig = NULL;
    MFCreateAttributes(&pReaderConfig, 1);
    pReaderConfig->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1);

    IMFSourceReader *pReader = NULL;
    hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderConfig, &pReader);
    SafeRelease(&pReaderConfig);

    if (FAILED(hr)) {
        SafeRelease(&pSource);
        webcam_log("Error creando SourceReader.");
        return NULL;
    }

    // Configurar Tipo de Salida Deseada
    IMFMediaType *pType = NULL;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    
    if (format == WEBCAM_FMT_YUYV) {
        pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
    } else {
        pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    
    MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
    
    hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
    
    // Si falla la resolucion exacta, intentar sin forzar resolucion (dejar que la camara decida)
    if (FAILED(hr)) {
        webcam_log("Resolucion exacta no soportada, intentando por defecto...");
        // Resetear tipo y solo pedir formato de color
        pType->DeleteAllItems();
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pType->SetGUID(MF_MT_SUBTYPE, (format == WEBCAM_FMT_YUYV) ? MFVideoFormat_YUY2 : MFVideoFormat_RGB32);
        hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
    }
    SafeRelease(&pType);

    if (FAILED(hr)) {
        webcam_log("No se pudo negociar el formato de video.");
        SafeRelease(&pReader);
        SafeRelease(&pSource);
        return NULL;
    }

    // Verificar que resolucion obtuvimos realmente
    IMFMediaType *pCurrentType = NULL;
    int final_w = width, final_h = height;
    if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType))) {
        MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, (UINT32*)&final_w, (UINT32*)&final_h);
        SafeRelease(&pCurrentType);
    }

    Webcam *cam = new Webcam();
    cam->reader = pReader;
    cam->actual_width = final_w;
    cam->actual_height = final_h;
    cam->format = format;
    
    // Interfaces de control
    pSource->QueryInterface(IID_PPV_ARGS(&cam->procAmp));
    pSource->QueryInterface(IID_PPV_ARGS(&cam->camControl));
    SafeRelease(&pSource); // El reader mantiene la referencia necesaria

    return cam;
}

WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame) {
    if (!cam || !cam->reader) return -1;
    
    IMFSample *pSample = NULL;
    DWORD flags = 0;
    HRESULT hr = cam->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, NULL, &pSample);
    
    if (FAILED(hr) || !pSample) return -1;

    IMFMediaBuffer *pBuffer = NULL;
    if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer))) {
        BYTE *pData = NULL;
        DWORD len = 0;
        pBuffer->Lock(&pData, NULL, &len);
        
        frame->width = cam->actual_width;
        frame->height = cam->actual_height;
        frame->format = cam->format;
        frame->timestamp_ms = GetTickCount();

        if (cam->format == WEBCAM_FMT_YUYV) {
            int needed = cam->actual_width * cam->actual_height * 2;
            if (len >= needed) {
                 memcpy(frame->data, pData, needed);
                 frame->size = needed;
            }
        } else {
            // RGB32 -> RGB24
            int pixels = cam->actual_width * cam->actual_height;
            if (len >= pixels * 4) {
                for (int i = 0; i < pixels; i++) {
                    frame->data[i*3]     = pData[i*4+2]; // R
                    frame->data[i*3+1]   = pData[i*4+1]; // G
                    frame->data[i*3+2]   = pData[i*4];   // B
                }
                frame->size = pixels * 3;
            }
        }

        pBuffer->Unlock();
        pBuffer->Release();
    }
    SafeRelease(&pSample);
    return 0;
}

WEBCAM_API void webcam_close(Webcam *cam) {
    if (cam) {
        SafeRelease(&cam->procAmp);
        SafeRelease(&cam->camControl);
        SafeRelease(&cam->reader);
        delete cam;
        // No llamamos a MFShutdown aqui para no romper si hay multiples instancias, 
        // normalmente se llama al cerrar la app, pero para DLL simple esta bien.
    }
}

WEBCAM_API int webcam_get_actual_width(Webcam *cam) { return cam ? cam->actual_width : 0; }
WEBCAM_API int webcam_get_actual_height(Webcam *cam) { return cam ? cam->actual_height : 0; }

// --- Implementacion de Controles ---
long get_proc_amp(Webcam *cam, long prop) {
    if (!cam->procAmp) return -1;
    long val, f; 
    return SUCCEEDED(cam->procAmp->Get(prop, &val, &f)) ? val : -1;
}
long get_cam_ctrl(Webcam *cam, long prop) {
    if (!cam->camControl) return -1;
    long val, f; 
    return SUCCEEDED(cam->camControl->Get(prop, &val, &f)) ? val : -1;
}
int set_proc_amp(Webcam *cam, long prop, long val, int is_auto) {
    if (!cam->procAmp) return -1;
    return SUCCEEDED(cam->procAmp->Set(prop, val, is_auto ? VideoProcAmp_Flags_Auto : VideoProcAmp_Flags_Manual)) ? 0 : -1;
}
int set_cam_ctrl(Webcam *cam, long prop, long val, int is_auto) {
    if (!cam->camControl) return -1;
    return SUCCEEDED(cam->camControl->Set(prop, val, is_auto ? CameraControl_Flags_Auto : CameraControl_Flags_Manual)) ? 0 : -1;
}

WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param) {
    switch(param) {
        case WEBCAM_PARAM_BRIGHTNESS: return get_proc_amp(cam, VideoProcAmp_Brightness);
        case WEBCAM_PARAM_CONTRAST:   return get_proc_amp(cam, VideoProcAmp_Contrast);
        case WEBCAM_PARAM_SATURATION: return get_proc_amp(cam, VideoProcAmp_Saturation);
        case WEBCAM_PARAM_EXPOSURE:   return get_cam_ctrl(cam, CameraControl_Exposure);
        case WEBCAM_PARAM_FOCUS:      return get_cam_ctrl(cam, CameraControl_Focus);
        case WEBCAM_PARAM_ZOOM:       return get_cam_ctrl(cam, CameraControl_Zoom);
    }
    return -1;
}

WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value) {
    switch(param) {
        case WEBCAM_PARAM_BRIGHTNESS: return set_proc_amp(cam, VideoProcAmp_Brightness, value, 0);
        case WEBCAM_PARAM_CONTRAST:   return set_proc_amp(cam, VideoProcAmp_Contrast, value, 0);
        case WEBCAM_PARAM_SATURATION: return set_proc_amp(cam, VideoProcAmp_Saturation, value, 0);
        case WEBCAM_PARAM_EXPOSURE:   return set_cam_ctrl(cam, CameraControl_Exposure, value, 0);
        case WEBCAM_PARAM_FOCUS:      return set_cam_ctrl(cam, CameraControl_Focus, value, 0);
        case WEBCAM_PARAM_ZOOM:       return set_cam_ctrl(cam, CameraControl_Zoom, value, 0);
    }
    return -1;
}

WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto) {
    // Para poner auto, el valor no importa tanto, pasamos 0
    switch(param) {
        case WEBCAM_PARAM_EXPOSURE:   return set_cam_ctrl(cam, CameraControl_Exposure, 0, is_auto);
        case WEBCAM_PARAM_FOCUS:      return set_cam_ctrl(cam, CameraControl_Focus, 0, is_auto);
    }
    return -1;
}

} // extern C