#include "webcam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>

struct Webcam {
    int fd;
    int actual_width;
    int actual_height;
    int stride;
    WebcamPixelFormat format;
    unsigned char *buffer_start;
    size_t buffer_length;
};

// Mapeo de parámetros V4L2
static int get_cid(WebcamParameter p) {
    switch(p) {
        case WEBCAM_PARAM_BRIGHTNESS: return V4L2_CID_BRIGHTNESS;
        case WEBCAM_PARAM_CONTRAST:   return V4L2_CID_CONTRAST;
        case WEBCAM_PARAM_SATURATION: return V4L2_CID_SATURATION;
        case WEBCAM_PARAM_EXPOSURE:   return V4L2_CID_EXPOSURE_ABSOLUTE;
        case WEBCAM_PARAM_FOCUS:      return V4L2_CID_FOCUS_ABSOLUTE;
        case WEBCAM_PARAM_ZOOM:       return V4L2_CID_ZOOM_ABSOLUTE;
        case WEBCAM_PARAM_GAIN:       return V4L2_CID_GAIN;
        default: return -1;
    }
}

WEBCAM_API WebcamInfo* webcam_list_devices(int *count) {
    WebcamInfo *list = calloc(64, sizeof(WebcamInfo));
    int found = 0;
    char path[20];

    for(int i=0; i<64; i++) {
        sprintf(path, "/dev/video%d", i);
        int fd = open(path, O_RDWR);
        if(fd != -1) {
            struct v4l2_capability cap;
            if(ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                if(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
                    list[found].index = i;
                    strncpy(list[found].name, (char*)cap.card, 127);
                    strcpy(list[found].path, path);
                    found++;
                }
            }
            close(fd);
        }
    }
    *count = found;
    if (found == 0) { free(list); return NULL; }
    return list;
}

WEBCAM_API void webcam_free_list(WebcamInfo *list) {
    if (list) free(list);
}

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index, WebcamPixelFormat format) {
    char dev_name[20];
    sprintf(dev_name, "/dev/video%d", device_index);

    int fd = open(dev_name, O_RDWR);
    if (fd == -1) return NULL;

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    
    // Mapeo de formatos
    switch(format) {
        case WEBCAM_FMT_YUYV:    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; break;
        case WEBCAM_FMT_RGB32:   fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB32; break;
        case WEBCAM_FMT_YUV420P: fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420; break;
        default:                 fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24; break;
    }

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) { close(fd); return NULL; }

    Webcam *cam = calloc(1, sizeof(Webcam));
    cam->fd = fd;
    cam->actual_width = fmt.fmt.pix.width;
    cam->actual_height = fmt.fmt.pix.height;
    cam->stride = fmt.fmt.pix.bytesperline;
    cam->format = format;

    struct v4l2_requestbuffers req = {0};
    req.count = 1; 
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &req);

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 
    buf.memory = V4L2_MEMORY_MMAP; 
    buf.index = 0;
    ioctl(fd, VIDIOC_QUERYBUF, &buf);

    cam->buffer_length = buf.length;
    cam->buffer_start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);

    // Encolar inicial
    ioctl(fd, VIDIOC_QBUF, &buf);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);

    return cam;
}

WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 
    buf.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) == -1) return -1;

    frame->data = cam->buffer_start; 
    frame->size = buf.bytesused;
    frame->width = cam->actual_width;
    frame->height = cam->actual_height;
    frame->stride = cam->stride;
    frame->format = cam->format;
    frame->timestamp_ms = (buf.timestamp.tv_sec * 1000) + (buf.timestamp.tv_usec / 1000);
    
    // Guardamos el indice en void* usando casting (intptr_t sería mejor pero void* es standard)
    frame->private_data[0] = (void*)(long)buf.index;

    return 0;
}

WEBCAM_API void webcam_release_frame(Webcam *cam, WebcamFrame *frame) {
    if (!cam) return;
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = (int)(long)frame->private_data[0];

    ioctl(cam->fd, VIDIOC_QBUF, &buf);
}

WEBCAM_API void webcam_close(Webcam *cam) {
    if (cam) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
        munmap(cam->buffer_start, cam->buffer_length);
        close(cam->fd);
        free(cam);
    }
}

// --- Controles (Igual que antes) ---
WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value) {
    struct v4l2_control ctrl = { .id = get_cid(param), .value = (int)value };
    if (ctrl.id == -1) return -1;
    return ioctl(cam->fd, VIDIOC_S_CTRL, &ctrl);
}
WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param) {
    struct v4l2_control ctrl = { .id = get_cid(param) };
    if (ctrl.id == -1) return -1;
    if (ioctl(cam->fd, VIDIOC_G_CTRL, &ctrl) == -1) return -1;
    return ctrl.value;
}
WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto) {
    if (param == WEBCAM_PARAM_EXPOSURE) {
        struct v4l2_control ctrl = { .id = V4L2_CID_EXPOSURE_AUTO, .value = is_auto ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL };
        return ioctl(cam->fd, VIDIOC_S_CTRL, &ctrl);
    }
    if (param == WEBCAM_PARAM_FOCUS) {
        struct v4l2_control ctrl = { .id = V4L2_CID_FOCUS_AUTO, .value = is_auto ? 1 : 0 };
        return ioctl(cam->fd, VIDIOC_S_CTRL, &ctrl);
    }
    return -1;
}
WEBCAM_API int webcam_get_actual_width(Webcam *cam) { return cam ? cam->actual_width : 0; }
WEBCAM_API int webcam_get_actual_height(Webcam *cam) { return cam ? cam->actual_height : 0; }