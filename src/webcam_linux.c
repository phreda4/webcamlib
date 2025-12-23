// ============================================================================
// webcam_linux.c - Linux Implementation with Zero-Copy
// ============================================================================
#ifdef __linux__

#include "webcam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/videodev2.h>

#define MAX_BUFFERS 4

struct Webcam {
    int fd;
    int actual_width;
    int actual_height;
    struct {
        void *start;
        size_t length;
    } buffers[MAX_BUFFERS];
    int buffer_count;
    int current_buffer_index;
    WebcamPixelFormat format;
};

WEBCAM_API WebcamInfo* webcam_list_devices(int *count) {
    WebcamInfo *temp_list = malloc(20 * sizeof(WebcamInfo));
    if (!temp_list) { *count = 0; return NULL; }
    
    int found = 0;
    char path[20];

    for(int i = 0; i < 20; i++) {
        sprintf(path, "/dev/video%d", i);
        int fd = open(path, O_RDWR);
        if(fd != -1) {
            struct v4l2_capability cap;
            if(ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                if(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
                    temp_list[found].index = i;
                    strncpy(temp_list[found].name, (char*)cap.card, 127);
                    temp_list[found].name[127] = '\0';
                    strcpy(temp_list[found].path, path);
                    found++;
                }
            }
            close(fd);
        }
    }
    *count = found;
    if(found == 0) { 
        free(temp_list); 
        return NULL; 
    }
    return realloc(temp_list, found * sizeof(WebcamInfo));
}

WEBCAM_API WebcamCapabilities* webcam_query_capabilities(int device_index) {
    char dev_name[20];
    sprintf(dev_name, "/dev/video%d", device_index);
    
    int fd = open(dev_name, O_RDWR);
    if (fd == -1) return NULL;
    
    WebcamCapabilities *caps = calloc(1, sizeof(WebcamCapabilities));
    if (!caps) { close(fd); return NULL; }
    
    struct v4l2_fmtdesc fmtdesc = {0};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    int max_formats = 200;
    WebcamFormatInfo *formats = malloc(max_formats * sizeof(WebcamFormatInfo));
    if (!formats) { close(fd); free(caps); return NULL; }
    
    int format_count = 0;
    caps->min_width = 99999;
    caps->min_height = 99999;
    caps->max_width = 0;
    caps->max_height = 0;
    
    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0 && format_count < max_formats) {
        WebcamPixelFormat fmt_type;
        int recognized = 1;
        
        switch (fmtdesc.pixelformat) {
            case V4L2_PIX_FMT_RGB24:
                fmt_type = WEBCAM_FMT_RGB24;
                break;
            case V4L2_PIX_FMT_RGB32:
                fmt_type = WEBCAM_FMT_RGB32;
                break;
            case V4L2_PIX_FMT_YUYV:
                fmt_type = WEBCAM_FMT_YUYV;
                break;
            case V4L2_PIX_FMT_YUV420:
                fmt_type = WEBCAM_FMT_YUV420;
                break;
            case V4L2_PIX_FMT_MJPEG:
                fmt_type = WEBCAM_FMT_MJPEG;
                break;
            default:
                recognized = 0;
                break;
        }
        
        if (recognized) {
            struct v4l2_frmsizeenum frmsize = {0};
            frmsize.pixel_format = fmtdesc.pixelformat;
            
            while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0 && 
                   format_count < max_formats) {
                if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    formats[format_count].format = fmt_type;
                    formats[format_count].width = frmsize.discrete.width;
                    formats[format_count].height = frmsize.discrete.height;
                    formats[format_count].fps = 30;
                    
                    if (frmsize.discrete.width > caps->max_width)
                        caps->max_width = frmsize.discrete.width;
                    if (frmsize.discrete.height > caps->max_height)
                        caps->max_height = frmsize.discrete.height;
                    if (frmsize.discrete.width < caps->min_width)
                        caps->min_width = frmsize.discrete.width;
                    if (frmsize.discrete.height < caps->min_height)
                        caps->min_height = frmsize.discrete.height;
                    
                    format_count++;
                }
                frmsize.index++;
            }
        }
        
        fmtdesc.index++;
    }
    
    close(fd);
    
    if (format_count == 0) {
        free(formats);
        free(caps);
        return NULL;
    }
    
    caps->formats = realloc(formats, format_count * sizeof(WebcamFormatInfo));
    caps->format_count = format_count;
    return caps;
}

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index,
                               WebcamPixelFormat format) {
    Webcam *cam = calloc(1, sizeof(Webcam));
    if (!cam) return NULL;
    
    char dev_name[20];
    sprintf(dev_name, "/dev/video%d", device_index);
    cam->fd = open(dev_name, O_RDWR);
    if (cam->fd == -1) { 
        free(cam); 
        return NULL; 
    }

    // Map format to V4L2 pixel format
    uint32_t v4l2_fmt;
    switch (format) {
        case WEBCAM_FMT_RGB24:
            v4l2_fmt = V4L2_PIX_FMT_RGB24;
            break;
        case WEBCAM_FMT_RGB32:
            v4l2_fmt = V4L2_PIX_FMT_RGB32;
            break;
        case WEBCAM_FMT_YUYV:
            v4l2_fmt = V4L2_PIX_FMT_YUYV;
            break;
        case WEBCAM_FMT_YUV420:
            v4l2_fmt = V4L2_PIX_FMT_YUV420;
            break;
        case WEBCAM_FMT_MJPEG:
            v4l2_fmt = V4L2_PIX_FMT_MJPEG;
            break;
        default:
            v4l2_fmt = V4L2_PIX_FMT_YUYV;
            break;
    }
    
    cam->format = format;

    // Set format
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = v4l2_fmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    
    if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) == -1) {
        close(cam->fd); 
        free(cam); 
        return NULL;
    }

    cam->actual_width = fmt.fmt.pix.width;
    cam->actual_height = fmt.fmt.pix.height;

    // Request buffers
    struct v4l2_requestbuffers req = {0};
    req.count = MAX_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) == -1) {
        close(cam->fd); 
        free(cam); 
        return NULL;
    }
    
    if (req.count < 1) {
        close(cam->fd); 
        free(cam); 
        return NULL;
    }
    
    cam->buffer_count = req.count;

    // Map and queue all buffers
    for (int i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            for (int j = 0; j < i; j++)
                munmap(cam->buffers[j].start, cam->buffers[j].length);
            close(cam->fd); 
            free(cam); 
            return NULL;
        }

        cam->buffers[i].length = buf.length;
        cam->buffers[i].start = mmap(NULL, buf.length, 
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, cam->fd, buf.m.offset);
        
        if (cam->buffers[i].start == MAP_FAILED) {
            for (int j = 0; j < i; j++)
                munmap(cam->buffers[j].start, cam->buffers[j].length);
            close(cam->fd); 
            free(cam); 
            return NULL;
        }

        if (ioctl(cam->fd, VIDIOC_QBUF, &buf) == -1) {
            for (int j = 0; j <= i; j++)
                munmap(cam->buffers[j].start, cam->buffers[j].length);
            close(cam->fd); 
            free(cam); 
            return NULL;
        }
    }

    // Start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam->fd, VIDIOC_STREAMON, &type) == -1) {
        for (int i = 0; i < cam->buffer_count; i++)
            munmap(cam->buffers[i].start, cam->buffers[i].length);
        close(cam->fd); 
        free(cam); 
        return NULL;
    }

    return cam;
}

WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame) {
    if (!cam || !frame) return -1;

    // Wait for frame
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    struct timeval tv = {2, 0};

    int r = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (r == -1) return -1;
    if (r == 0) return -2; // Timeout

    // Dequeue buffer
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) == -1) {
        return -1;
    }

    cam->current_buffer_index = buf.index;

    // Fill frame info (ZERO-COPY)
    frame->data = (const unsigned char*)cam->buffers[buf.index].start;
    frame->width = cam->actual_width;
    frame->height = cam->actual_height;
    frame->format = cam->format;
    frame->timestamp_ms = (buf.timestamp.tv_sec * 1000) + 
                         (buf.timestamp.tv_usec / 1000);
    
    // Calculate size based on format
    switch (cam->format) {
        case WEBCAM_FMT_RGB24:
            frame->size = cam->actual_width * cam->actual_height * 3;
            break;
        case WEBCAM_FMT_RGB32:
            frame->size = cam->actual_width * cam->actual_height * 4;
            break;
        case WEBCAM_FMT_YUYV:
            frame->size = cam->actual_width * cam->actual_height * 2;
            break;
        case WEBCAM_FMT_YUV420:
            frame->size = cam->actual_width * cam->actual_height * 3 / 2;
            break;
        case WEBCAM_FMT_MJPEG:
            frame->size = buf.bytesused;
            break;
        default:
            frame->size = buf.bytesused;
    }

    return 0;
}

WEBCAM_API void webcam_release_frame(Webcam *cam) {
    if (!cam) return;
    
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = cam->current_buffer_index;
    
    ioctl(cam->fd, VIDIOC_QBUF, &buf);
}

WEBCAM_API void webcam_close(Webcam *cam) {
    if (cam) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
        
        for (int i = 0; i < cam->buffer_count; i++)
            munmap(cam->buffers[i].start, cam->buffers[i].length);
        
        close(cam->fd);
        free(cam);
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

WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param) {
    if (!cam) return -1;
    struct v4l2_control ctrl = {0};
    switch(param) {
        case WEBCAM_PARAM_BRIGHTNESS: ctrl.id = V4L2_CID_BRIGHTNESS; break;
        case WEBCAM_PARAM_CONTRAST: ctrl.id = V4L2_CID_CONTRAST; break;
        case WEBCAM_PARAM_SATURATION: ctrl.id = V4L2_CID_SATURATION; break;
        case WEBCAM_PARAM_EXPOSURE: ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE; break;
        case WEBCAM_PARAM_FOCUS: ctrl.id = V4L2_CID_FOCUS_ABSOLUTE; break;
        case WEBCAM_PARAM_ZOOM: ctrl.id = V4L2_CID_ZOOM_ABSOLUTE; break;
        case WEBCAM_PARAM_GAIN: ctrl.id = V4L2_CID_GAIN; break;
        case WEBCAM_PARAM_SHARPNESS: ctrl.id = V4L2_CID_SHARPNESS; break;
        default: return -1;
    }
    return (ioctl(cam->fd, VIDIOC_G_CTRL, &ctrl) == 0) ? ctrl.value : -1;
}

WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value) {
    if (!cam) return -1;
    struct v4l2_control ctrl = {0};
    ctrl.value = value;
    switch(param) {
        case WEBCAM_PARAM_BRIGHTNESS: ctrl.id = V4L2_CID_BRIGHTNESS; break;
        case WEBCAM_PARAM_CONTRAST: ctrl.id = V4L2_CID_CONTRAST; break;
        case WEBCAM_PARAM_SATURATION: ctrl.id = V4L2_CID_SATURATION; break;
        case WEBCAM_PARAM_EXPOSURE: ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE; break;
        case WEBCAM_PARAM_FOCUS: ctrl.id = V4L2_CID_FOCUS_ABSOLUTE; break;
        case WEBCAM_PARAM_ZOOM: ctrl.id = V4L2_CID_ZOOM_ABSOLUTE; break;
        case WEBCAM_PARAM_GAIN: ctrl.id = V4L2_CID_GAIN; break;
        case WEBCAM_PARAM_SHARPNESS: ctrl.id = V4L2_CID_SHARPNESS; break;
        default: return -1;
    }
    return (ioctl(cam->fd, VIDIOC_S_CTRL, &ctrl) == 0) ? 0 : -1;
}

WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto) {
    if (!cam) return -1;
    struct v4l2_control ctrl = {0};
    switch(param) {
        case WEBCAM_PARAM_EXPOSURE:
            ctrl.id = V4L2_CID_EXPOSURE_AUTO;
            ctrl.value = is_auto ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL;
            break;
        case WEBCAM_PARAM_FOCUS:
            ctrl.id = V4L2_CID_FOCUS_AUTO;
            ctrl.value = is_auto ? 1 : 0;
            break;
        default:
            return -1;
    }
    return (ioctl(cam->fd, VIDIOC_S_CTRL, &ctrl) == 0) ? 0 : -1;
}

#endif // __linux__
