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
#include <errno.h>

extern void webcam_log(const char *format, ...);

struct Webcam {
    int fd;
    int actual_width;
    int actual_height;
    unsigned char *buffer_start;
    size_t buffer_length;
    WebcamPixelFormat format;
};

// ... [Funcion yuyv_to_rgb identica a la anterior] ...
static void yuyv_to_rgb(unsigned char *yuyv, unsigned char *rgb, int width, int height) {
    int i, j, y0, u, y1, v, r, g, b;
    for (i = 0, j = 0; i < (width * height * 2); i += 4, j += 6) {
        y0 = yuyv[i]; u = yuyv[i + 1] - 128; y1 = yuyv[i + 2]; v = yuyv[i + 3] - 128;
        r = y0 + (1.402 * v); g = y0 - (0.344 * u) - (0.714 * v); b = y0 + (1.772 * u);
        rgb[j] = (r<0?0:(r>255?255:r)); rgb[j+1] = (g<0?0:(g>255?255:g)); rgb[j+2] = (b<0?0:(b>255?255:b));
        r = y1 + (1.402 * v); g = y1 - (0.344 * u) - (0.714 * v); b = y1 + (1.772 * u);
        rgb[j+3] = (r<0?0:(r>255?255:r)); rgb[j+4] = (g<0?0:(g>255?255:g)); rgb[j+5] = (b<0?0:(b>255?255:b));
    }
}

WEBCAM_API WebcamInfo* webcam_list_devices(int *count) {
    WebcamInfo *temp_list = malloc(20 * sizeof(WebcamInfo));
    int found = 0;
    char path[20];

    for(int i=0; i<20; i++) {
        sprintf(path, "/dev/video%d", i);
        int fd = open(path, O_RDWR);
        if(fd != -1) {
            struct v4l2_capability cap;
            if(ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                if(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
                    temp_list[found].index = i;
                    strncpy(temp_list[found].name, (char*)cap.card, 127);
                    strcpy(temp_list[found].path, path);
                    found++;
                }
            }
            close(fd);
        }
    }
    *count = found;
    if(found == 0) { free(temp_list); return NULL; }
    return realloc(temp_list, found * sizeof(WebcamInfo));
}

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index, WebcamPixelFormat format) {
    Webcam *cam = malloc(sizeof(Webcam));
    char dev_name[20];
    sprintf(dev_name, "/dev/video%d", device_index);

    cam->fd = open(dev_name, O_RDWR);
    if (cam->fd == -1) { 
        webcam_log("No se pudo abrir %s", dev_name);
        free(cam); 
        return NULL; 
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; 
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    
    if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) == -1) {
        webcam_log("Fallo al setear formato YUYV.");
        close(cam->fd); free(cam); return NULL;
    }

    cam->actual_width = fmt.fmt.pix.width;
    cam->actual_height = fmt.fmt.pix.height;
    cam->format = format;

    struct v4l2_requestbuffers req = {0};
    req.count = 1; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
    ioctl(cam->fd, VIDIOC_REQBUFS, &req);

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = 0;
    ioctl(cam->fd, VIDIOC_QUERYBUF, &buf);

    cam->buffer_start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam->fd, buf.m.offset);
    cam->buffer_length = buf.length;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMON, &type);

    return cam;
}

WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    struct timeval tv = {2, 0}; // Timeout de 2 segundos

    int r = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (r == -1) return -1; // Error
    if (r == 0) return -2;  // Timeout

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(cam->fd, VIDIOC_QBUF, &buf) == -1) return -1;
    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) == -1) return -1;

    frame->width = cam->actual_width;
    frame->height = cam->actual_height;
    frame->format = cam->format;
    frame->timestamp_ms = (buf.timestamp.tv_sec * 1000) + (buf.timestamp.tv_usec / 1000);

    if (cam->format == WEBCAM_FMT_YUYV) {
        frame->size = cam->actual_width * cam->actual_height * 2;
        memcpy(frame->data, cam->buffer_start, frame->size);
    } else {
        frame->size = cam->actual_width * cam->actual_height * 3;
        yuyv_to_rgb(cam->buffer_start, frame->data, cam->actual_width, cam->actual_height);
    }
    
    return 0;
}

WEBCAM_API void webcam_close(Webcam *cam) {
    if (cam) {
        munmap(cam->buffer_start, cam->buffer_length);
        close(cam->fd);
        free(cam);
    }
}
WEBCAM_API int webcam_get_actual_width(Webcam *cam) { return cam ? cam->actual_width : 0; }
WEBCAM_API int webcam_get_actual_height(Webcam *cam) { return cam ? cam->actual_height : 0; }

// Stubs para controles (se deben pegar los del ejemplo anterior aqui)
WEBCAM_API long webcam_get_parameter(Webcam *cam, WebcamParameter param) { return -1; }
WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value) { return -1; }
WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto) { return -1; }