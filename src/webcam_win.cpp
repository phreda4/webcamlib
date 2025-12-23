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
    IMFSample *current_sample;
    IMFMediaBuffer *current_buffer;
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
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, 
                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    
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
        
        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, 
                                         &name, NULL);
        ppDevices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym, NULL);
        
        if (name) { 
            wchar_to_char(name, list[i].name, 128); 
            CoTaskMemFree(name); 
        }
        if (sym) { 
            wchar_to_char(sym, list[i].path, 256); 
            CoTaskMemFree(sym); 
        }
        
        SafeRelease(&ppDevices[i]);
    }
    CoTaskMemFree(ppDevices);
    return list;
}

WEBCAM_API WebcamCapabilities* webcam_query_capabilities(int device_index) {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return NULL;
    
    IMFAttributes *pConfig = NULL;
    IMFActivate **ppDevices = NULL;
    UINT32 count = 0;
    
    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, 
                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    MFEnumDeviceSources(pConfig, &ppDevices, &count);
    pConfig->Release();

    if (device_index >= (int)count) {
        CoTaskMemFree(ppDevices);
        return NULL;
    }

    IMFMediaSource *pSource = NULL;
    hr = ppDevices[device_index]->ActivateObject(IID_PPV_ARGS(&pSource));
    
    for(UINT32 i = 0; i < count; i++) SafeRelease(&ppDevices[i]);
    CoTaskMemFree(ppDevices);

    if (FAILED(hr)) return NULL;

    IMFSourceReader *pReader = NULL;
    hr = MFCreateSourceReaderFromMediaSource(pSource, NULL, &pReader);
    SafeRelease(&pSource);

    if (FAILED(hr)) return NULL;

    WebcamCapabilities *caps = (WebcamCapabilities*)calloc(1, sizeof(WebcamCapabilities));
    WebcamFormatInfo *formats = (WebcamFormatInfo*)malloc(100 * sizeof(WebcamFormatInfo));
    int format_count = 0;

    caps->min_width = 99999;
    caps->min_height = 99999;

    DWORD dwIndex = 0;
    IMFMediaType *pType = NULL;
    
    while (SUCCEEDED(pReader->GetNativeMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, dwIndex, &pType))) {
        
        GUID subtype;
        UINT32 w, h;
        
                    if (SUCCEEDED(pType->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            SUCCEEDED(MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &w, &h))) {
            
            WebcamPixelFormat fmt;
            int recognized = 1;
            
            if (IsEqualGUID(subtype, MFVideoFormat_RGB24)) fmt = WEBCAM_FMT_RGB24;
            else if (IsEqualGUID(subtype, MFVideoFormat_RGB32)) fmt = WEBCAM_FMT_RGB32;
            else if (IsEqualGUID(subtype, MFVideoFormat_YUY2)) fmt = WEBCAM_FMT_YUYV;
            else if (IsEqualGUID(subtype, MFVideoFormat_I420)) fmt = WEBCAM_FMT_YUV420;
            else if (IsEqualGUID(subtype, MFVideoFormat_MJPG)) fmt = WEBCAM_FMT_MJPEG;
            else recognized = 0;
            
            if (recognized && format_count < 100) {
                formats[format_count].format = fmt;
                formats[format_count].width = w;
                formats[format_count].height = h;
                formats[format_count].fps = 30;
                
                if (w > caps->max_width) caps->max_width = w;
                if (h > caps->max_height) caps->max_height = h;
                if (w < caps->min_width) caps->min_width = w;
                if (h < caps->min_height) caps->min_height = h;
                
                format_count++;
            }
        }
        
        SafeRelease(&pType);
        dwIndex++;
    }
    
    SafeRelease(&pReader);
    
    caps->formats = (WebcamFormatInfo*)realloc(formats, format_count * sizeof(WebcamFormatInfo));
    caps->format_count = format_count;
    return caps;
}

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index,
                               WebcamPixelFormat format) {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return NULL;
    
    IMFAttributes *pConfig = NULL;
    IMFActivate **ppDevices = NULL;
    UINT32 count = 0;
    
    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, 
                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    MFEnumDeviceSources(pConfig, &ppDevices, &count);
    pConfig->Release();

    if (device_index >= (int)count) {
        CoTaskMemFree(ppDevices);
        return NULL;
    }

    IMFMediaSource *pSource = NULL;
    hr = ppDevices[device_index]->ActivateObject(IID_PPV_ARGS(&pSource));
    
    for(UINT32 i = 0; i < count; i++) SafeRelease(&ppDevices[i]);
    CoTaskMemFree(ppDevices);
    if (FAILED(hr)) return NULL;

    IMFSourceReader *pReader = NULL;
    hr = MFCreateSourceReaderFromMediaSource(pSource, NULL, &pReader);

    if (FAILED(hr)) {
        SafeRelease(&pSource);
        return NULL;
    }

    IMFMediaType *pType = NULL;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    
    switch (format) {
        case WEBCAM_FMT_RGB24:
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);
            break;
        case WEBCAM_FMT_RGB32:
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            break;
        case WEBCAM_FMT_YUYV:
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
            break;
        case WEBCAM_FMT_YUV420:
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420);
            break;
        case WEBCAM_FMT_MJPEG:
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MJPG);
            break;
    }
    
    MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
    hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
    
    if (FAILED(hr)) {
        pType->DeleteAllItems();
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        
        switch (format) {
            case WEBCAM_FMT_RGB24:
                pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);
                break;
            case WEBCAM_FMT_RGB32:
                pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                break;
            case WEBCAM_FMT_YUYV:
                pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
                break;
            case WEBCAM_FMT_YUV420:
                pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_I420);
                break;
            case WEBCAM_FMT_MJPEG:
                pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MJPG);
                break;
        }
        
        hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
    }
    SafeRelease(&pType);

    if (FAILED(hr)) {
        SafeRelease(&pReader);
        SafeRelease(&pSource);
        return NULL;
    }

    IMFMediaType *pCurrentType = NULL;
    int final_w = width, final_h = height;
    if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 
                                                &pCurrentType))) {
        MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, 
                          (UINT32*)&final_w, (UINT32*)&final_h);
        SafeRelease(&pCurrentType);
    }

    Webcam *cam = new Webcam();
    cam->reader = pReader;
    cam->actual_width = final_w;
    cam->actual_height = final_h;
    cam->format = format;
    cam->current_sample = NULL;
    cam->current_buffer = NULL;
    
    pSource->QueryInterface(IID_PPV_ARGS(&cam->procAmp));
    pSource->QueryInterface(IID_PPV_ARGS(&cam->camControl));
    SafeRelease(&pSource);

    return cam;
}

WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame) {
    if (!cam || !cam->reader || !frame) return -1;
    
    // Release previous sample if any
    SafeRelease(&cam->current_buffer);
    SafeRelease(&cam->current_sample);
    
    DWORD flags = 0;
    HRESULT hr = cam->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 
                                         0, NULL, &flags, NULL, &cam->current_sample);
    
    if (FAILED(hr) || !cam->current_sample) return -1;

    if (SUCCEEDED(cam->current_sample->ConvertToContiguousBuffer(&cam->current_buffer))) {
        BYTE *pData = NULL;
        DWORD len = 0;
        cam->current_buffer->Lock(&pData, NULL, &len);
        
        // ZERO-COPY: point directly to buffer
        frame->data = (const unsigned char*)pData;
        frame->width = cam->actual_width;
        frame->height = cam->actual_height;
        frame->format = cam->format;
        frame->timestamp_ms = GetTickCount64();
        
        int pixels = cam->actual_width * cam->actual_height;
        
        switch (cam->format) {
            case WEBCAM_FMT_RGB24:
                frame->size = pixels * 3;
                break;
            case WEBCAM_FMT_RGB32:
                frame->size = pixels * 4;
                break;
            case WEBCAM_FMT_YUYV:
                frame->size = pixels * 2;
                break;
            case WEBCAM_FMT_YUV420:
                frame->size = pixels * 3 / 2;
                break;
            case WEBCAM_FMT_MJPEG:
                frame->size = len;
                break;
        }
        
        return 0;
    }
    
    return -1;
}

WEBCAM_API void webcam_release_frame(Webcam *cam) {
    if (!cam) return;
    if (cam->current_buffer) {
        cam->current_buffer->Unlock();
    }
}

WEBCAM_API void webcam_close(Webcam *cam) {
    if (cam) {
        if (cam->current_buffer) {
            cam->current_buffer->Unlock();
        }
        SafeRelease(&cam->current_buffer);
        SafeRelease(&cam->current_sample);
        SafeRelease(&cam->procAmp);
        SafeRelease(&cam->camControl);
        SafeRelease(&cam->reader);
        delete cam;
    }
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
    switch(param) {
        case WEBCAM_PARAM_BRIGHTNESS: return get_proc_amp(cam, VideoProcAmp_Brightness);
        case WEBCAM_PARAM_CONTRAST: return get_proc_amp(cam, VideoProcAmp_Contrast);
        case WEBCAM_PARAM_SATURATION: return get_proc_amp(cam, VideoProcAmp_Saturation);
        case WEBCAM_PARAM_SHARPNESS: return get_proc_amp(cam, VideoProcAmp_Sharpness);
        case WEBCAM_PARAM_GAIN: return get_proc_amp(cam, VideoProcAmp_Gain);
        case WEBCAM_PARAM_EXPOSURE: return get_cam_ctrl(cam, CameraControl_Exposure);
        case WEBCAM_PARAM_FOCUS: return get_cam_ctrl(cam, CameraControl_Focus);
        case WEBCAM_PARAM_ZOOM: return get_cam_ctrl(cam, CameraControl_Zoom);
    }
    return -1;
}

WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value) {
    if (!cam) return -1;
    switch(param) {
        case WEBCAM_PARAM_BRIGHTNESS: return set_proc_amp(cam, VideoProcAmp_Brightness, value, 0);
        case WEBCAM_PARAM_CONTRAST: return set_proc_amp(cam, VideoProcAmp_Contrast, value, 0);
        case WEBCAM_PARAM_SATURATION: return set_proc_amp(cam, VideoProcAmp_Saturation, value, 0);
        case WEBCAM_PARAM_SHARPNESS: return set_proc_amp(cam, VideoProcAmp_Sharpness, value, 0);
        case WEBCAM_PARAM_GAIN: return set_proc_amp(cam, VideoProcAmp_Gain, value, 0);
        case WEBCAM_PARAM_EXPOSURE: return set_cam_ctrl(cam, CameraControl_Exposure, value, 0);
        case WEBCAM_PARAM_FOCUS: return set_cam_ctrl(cam, CameraControl_Focus, value, 0);
        case WEBCAM_PARAM_ZOOM: return set_cam_ctrl(cam, CameraControl_Zoom, value, 0);
    }
    return -1;
}

WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto) {
    if (!cam) return -1;
    switch(param) {
        case WEBCAM_PARAM_EXPOSURE: return set_cam_ctrl(cam, CameraControl_Exposure, 0, is_auto);
        case WEBCAM_PARAM_FOCUS: return set_cam_ctrl(cam, CameraControl_Focus, 0, is_auto);
    }
    return -1;
}

} // extern "C"

#endif // _WIN32