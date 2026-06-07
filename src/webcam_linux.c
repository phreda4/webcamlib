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

#define MAX_BUFFERS  4
#define DEV_PATH_LEN 32

/* Definido en webcam_common.c */
extern size_t webcam_calc_frame_size(WebcamPixelFormat format,
                                      int width, int height,
                                      size_t bytesused);

struct Webcam {
    int fd;
    int actual_width;
    int actual_height;
    struct {
        void  *start;
        size_t length;
    } buffers[MAX_BUFFERS];
    int buffer_count;
    int current_buffer_index;   /* -1 = ningún buffer retenido */
    WebcamPixelFormat format;
};

/* ── Helpers de conversión de formato ──────────────────────────────────── */

static uint32_t webcam_to_v4l2(WebcamPixelFormat f) {
    switch (f) {
        case WEBCAM_FMT_RGB24:  return V4L2_PIX_FMT_RGB24;
        case WEBCAM_FMT_RGB32:  return V4L2_PIX_FMT_RGB32;
        case WEBCAM_FMT_YUYV:   return V4L2_PIX_FMT_YUYV;
        case WEBCAM_FMT_YUV420: return V4L2_PIX_FMT_YUV420;
        case WEBCAM_FMT_MJPEG:  return V4L2_PIX_FMT_MJPEG;
        default:                return V4L2_PIX_FMT_YUYV;
    }
}

/* Devuelve 1 si el formato V4L2 es reconocido, 0 si no */
static int v4l2_to_webcam(uint32_t pf, WebcamPixelFormat *out) {
    switch (pf) {
        case V4L2_PIX_FMT_RGB24:  *out = WEBCAM_FMT_RGB24;  return 1;
        case V4L2_PIX_FMT_RGB32:  *out = WEBCAM_FMT_RGB32;  return 1;
        case V4L2_PIX_FMT_YUYV:   *out = WEBCAM_FMT_YUYV;   return 1;
        case V4L2_PIX_FMT_YUV420: *out = WEBCAM_FMT_YUV420; return 1;
        case V4L2_PIX_FMT_MJPEG:  *out = WEBCAM_FMT_MJPEG;  return 1;
        default:                  return 0;
    }
}

/*
 * query_max_fps
 * -------------
 * Consulta VIDIOC_ENUM_FRAMEINTERVALS para obtener el FPS máximo
 * soportado por un formato/resolución dado. Retorna 30 si falla.
 *
 * Nota: el intervalo de frame es 1/fps (numerator/denominator segundos),
 * por lo que el menor intervalo corresponde al mayor FPS.
 */
static int query_max_fps(int fd, uint32_t pixfmt, uint32_t w, uint32_t h) {
    struct v4l2_frmivalenum fi = {0};
    fi.pixel_format = pixfmt;
    fi.width        = w;
    fi.height       = h;

    int best_fps = 30;

    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0) {
        int fps = 30;

        if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            if (fi.discrete.numerator > 0)
                fps = fi.discrete.denominator / fi.discrete.numerator;
        } else {
            /* CONTINUOUS / STEPWISE: el menor intervalo es el mayor FPS */
            if (fi.stepwise.min.numerator > 0)
                fps = fi.stepwise.min.denominator / fi.stepwise.min.numerator;
        }

        if (fps > best_fps) best_fps = fps;

        /* Para stepwise/continuous hay una sola entrada */
        if (fi.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
        fi.index++;
    }

    return best_fps;
}

/*
 * add_format_entry
 * ----------------
 * Agrega una entrada al array de formatos y actualiza min/max resolución.
 */
static void add_format_entry(WebcamFormatInfo *formats, int *count,
                              WebcamCapabilities *caps,
                              WebcamPixelFormat fmt_type,
                              uint32_t w, uint32_t h, int fps) {
    formats[*count].format = fmt_type;
    formats[*count].width  = w;
    formats[*count].height = h;
    formats[*count].fps    = fps;

    if (w > (uint32_t)caps->max_width)  caps->max_width  = w;
    if (h > (uint32_t)caps->max_height) caps->max_height = h;
    if (w < (uint32_t)caps->min_width)  caps->min_width  = w;
    if (h < (uint32_t)caps->min_height) caps->min_height = h;

    (*count)++;
}

/* ── API ────────────────────────────────────────────────────────────────── */

WEBCAM_API WebcamInfo* webcam_list_devices(int *count) {
    WebcamInfo *temp = malloc(20 * sizeof(WebcamInfo));
    if (!temp) { *count = 0; return NULL; }

    int found = 0;
    char path[DEV_PATH_LEN];

    for (int i = 0; i < 20; i++) {
        snprintf(path, sizeof(path), "/dev/video%d", i);

        /* O_RDONLY | O_NONBLOCK: no bloquear si otro proceso tiene el device */
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
            (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
            temp[found].index = i;
            strncpy(temp[found].name, (char*)cap.card, 127);
            temp[found].name[127] = '\0';
            strncpy(temp[found].path, path, sizeof(temp[found].path) - 1);
            found++;
        }
        close(fd);
    }

    *count = found;
    if (found == 0) { free(temp); return NULL; }
    return realloc(temp, found * sizeof(WebcamInfo));
}

WEBCAM_API WebcamCapabilities* webcam_query_capabilities(int device_index) {
    char dev_name[DEV_PATH_LEN];
    snprintf(dev_name, sizeof(dev_name), "/dev/video%d", device_index);

    /* O_RDONLY | O_NONBLOCK: solo necesitamos leer capacidades */
    int fd = open(dev_name, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;

    WebcamCapabilities *caps = calloc(1, sizeof(WebcamCapabilities));
    if (!caps) { close(fd); return NULL; }

    int max_formats = 200;
    WebcamFormatInfo *formats = malloc(max_formats * sizeof(WebcamFormatInfo));
    if (!formats) { free(caps); close(fd); return NULL; }

    int format_count = 0;
    caps->min_width  = 99999;
    caps->min_height = 99999;

    struct v4l2_fmtdesc fmtdesc = {0};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0 && format_count < max_formats) {
        WebcamPixelFormat fmt_type;
        if (!v4l2_to_webcam(fmtdesc.pixelformat, &fmt_type)) {
            fmtdesc.index++;
            continue;
        }

        struct v4l2_frmsizeenum frmsize = {0};
        frmsize.pixel_format = fmtdesc.pixelformat;

        while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0 &&
               format_count < max_formats) {

            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                uint32_t w = frmsize.discrete.width;
                uint32_t h = frmsize.discrete.height;
                int fps = query_max_fps(fd, fmtdesc.pixelformat, w, h);
                add_format_entry(formats, &format_count, caps, fmt_type, w, h, fps);
                frmsize.index++;

            } else {
                /*
                 * STEPWISE / CONTINUOUS: el driver acepta un rango continuo
                 * de resoluciones. Registramos mínimo y máximo como
                 * representantes para que find_best_format pueda operar.
                 */
                uint32_t ws[2] = { frmsize.stepwise.min_width,
                                   frmsize.stepwise.max_width  };
                uint32_t hs[2] = { frmsize.stepwise.min_height,
                                   frmsize.stepwise.max_height };
                for (int s = 0; s < 2 && format_count < max_formats; s++) {
                    int fps = query_max_fps(fd, fmtdesc.pixelformat, ws[s], hs[s]);
                    add_format_entry(formats, &format_count, caps,
                                     fmt_type, ws[s], hs[s], fps);
                }
                break; /* una sola entrada para stepwise/continuous */
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

    caps->formats      = realloc(formats, format_count * sizeof(WebcamFormatInfo));
    caps->format_count = format_count;
    return caps;
}

WEBCAM_API Webcam* webcam_open(int width, int height, int device_index,
                               WebcamPixelFormat format) {
    Webcam *cam = calloc(1, sizeof(Webcam));
    if (!cam) return NULL;

    cam->current_buffer_index = -1;   /* ningún buffer retenido al inicio */

    char dev_name[DEV_PATH_LEN];
    snprintf(dev_name, sizeof(dev_name), "/dev/video%d", device_index);
    cam->fd = open(dev_name, O_RDWR);
    if (cam->fd < 0) { free(cam); return NULL; }

    /* Configurar formato de video */
    struct v4l2_format fmt = {0};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = webcam_to_v4l2(format);
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        close(cam->fd); free(cam); return NULL;
    }

    cam->actual_width  = fmt.fmt.pix.width;
    cam->actual_height = fmt.fmt.pix.height;
    cam->format        = format;

    /* Solicitar buffers mmap */
    struct v4l2_requestbuffers req = {0};
    req.count  = MAX_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
        close(cam->fd); free(cam); return NULL;
    }
    cam->buffer_count = req.count;

    /* Mapear y encolar todos los buffers */
    for (int i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            for (int j = 0; j < i; j++)
                munmap(cam->buffers[j].start, cam->buffers[j].length);
            close(cam->fd); free(cam); return NULL;
        }

        cam->buffers[i].length = buf.length;
        cam->buffers[i].start  = mmap(NULL, buf.length,
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED, cam->fd, buf.m.offset);

        if (cam->buffers[i].start == MAP_FAILED) {
            for (int j = 0; j < i; j++)
                munmap(cam->buffers[j].start, cam->buffers[j].length);
            close(cam->fd); free(cam); return NULL;
        }

        if (ioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            for (int j = 0; j <= i; j++)
                munmap(cam->buffers[j].start, cam->buffers[j].length);
            close(cam->fd); free(cam); return NULL;
        }
    }

    /* Iniciar streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        for (int i = 0; i < cam->buffer_count; i++)
            munmap(cam->buffers[i].start, cam->buffers[i].length);
        close(cam->fd); free(cam); return NULL;
    }

    return cam;
}

WEBCAM_API int webcam_capture(Webcam *cam, WebcamFrame *frame) {
    if (!cam || !frame) return -1;

    /*
     * BUG FIX: auto-liberar el buffer anterior antes de pedir uno nuevo.
     *
     * Sin esto, cada llamada a webcam_capture sin webcam_release_frame previo
     * consume un slot del ring buffer. Con MAX_BUFFERS=4, al 5° frame
     * VIDIOC_DQBUF bloquea indefinidamente (starvation).
     *
     * Efecto observable: la versión original "funciona" los primeros 4 frames
     * y luego congela. Solo se notaba en uso real, no en tests breves.
     */
    if (cam->current_buffer_index >= 0) {
        struct v4l2_buffer prev = {0};
        prev.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        prev.memory = V4L2_MEMORY_MMAP;
        prev.index  = cam->current_buffer_index;
        ioctl(cam->fd, VIDIOC_QBUF, &prev);
        cam->current_buffer_index = -1;
    }

    /* Esperar frame disponible (timeout 2 segundos) */
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    struct timeval tv = {2, 0};

    int r = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (r < 0)  return -1;
    if (r == 0) return -2; /* timeout */

    /* Desencolar buffer */
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) return -1;

    cam->current_buffer_index = buf.index;

    /* Llenar frame info (ZERO-COPY: apunta directo al buffer mapeado) */
    frame->data         = (const unsigned char*)cam->buffers[buf.index].start;
    frame->width        = cam->actual_width;
    frame->height       = cam->actual_height;
    frame->format       = cam->format;
    frame->timestamp_ms = (buf.timestamp.tv_sec  * 1000ULL) +
                          (buf.timestamp.tv_usec / 1000);
    frame->size         = webcam_calc_frame_size(cam->format,
                              cam->actual_width, cam->actual_height,
                              buf.bytesused);
    return 0;
}

WEBCAM_API void webcam_release_frame(Webcam *cam) {
    if (!cam || cam->current_buffer_index < 0) return;

    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = cam->current_buffer_index;

    ioctl(cam->fd, VIDIOC_QBUF, &buf);
    cam->current_buffer_index = -1;  /* marcar como liberado */
}

WEBCAM_API void webcam_close(Webcam *cam) {
    if (!cam) return;

    /*
     * VIDIOC_STREAMOFF devuelve todos los buffers al estado "dequeued"
     * (la app los "posee" de nuevo), por lo que no necesitamos
     * hacer VIDIOC_QBUF del buffer retenido antes de parar.
     */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < cam->buffer_count; i++)
        munmap(cam->buffers[i].start, cam->buffers[i].length);

    close(cam->fd);
    free(cam);
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
    switch (param) {
        case WEBCAM_PARAM_BRIGHTNESS: ctrl.id = V4L2_CID_BRIGHTNESS;        break;
        case WEBCAM_PARAM_CONTRAST:   ctrl.id = V4L2_CID_CONTRAST;          break;
        case WEBCAM_PARAM_SATURATION: ctrl.id = V4L2_CID_SATURATION;        break;
        case WEBCAM_PARAM_EXPOSURE:   ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE; break;
        case WEBCAM_PARAM_FOCUS:      ctrl.id = V4L2_CID_FOCUS_ABSOLUTE;    break;
        case WEBCAM_PARAM_ZOOM:       ctrl.id = V4L2_CID_ZOOM_ABSOLUTE;     break;
        case WEBCAM_PARAM_GAIN:       ctrl.id = V4L2_CID_GAIN;              break;
        case WEBCAM_PARAM_SHARPNESS:  ctrl.id = V4L2_CID_SHARPNESS;        break;
        default: return -1;
    }
    return (ioctl(cam->fd, VIDIOC_G_CTRL, &ctrl) == 0) ? ctrl.value : -1;
}

WEBCAM_API int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value) {
    if (!cam) return -1;
    struct v4l2_control ctrl = {0};
    ctrl.value = value;
    switch (param) {
        case WEBCAM_PARAM_BRIGHTNESS: ctrl.id = V4L2_CID_BRIGHTNESS;        break;
        case WEBCAM_PARAM_CONTRAST:   ctrl.id = V4L2_CID_CONTRAST;          break;
        case WEBCAM_PARAM_SATURATION: ctrl.id = V4L2_CID_SATURATION;        break;
        case WEBCAM_PARAM_EXPOSURE:   ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE; break;
        case WEBCAM_PARAM_FOCUS:      ctrl.id = V4L2_CID_FOCUS_ABSOLUTE;    break;
        case WEBCAM_PARAM_ZOOM:       ctrl.id = V4L2_CID_ZOOM_ABSOLUTE;     break;
        case WEBCAM_PARAM_GAIN:       ctrl.id = V4L2_CID_GAIN;              break;
        case WEBCAM_PARAM_SHARPNESS:  ctrl.id = V4L2_CID_SHARPNESS;        break;
        default: return -1;
    }
    return (ioctl(cam->fd, VIDIOC_S_CTRL, &ctrl) == 0) ? 0 : -1;
}

WEBCAM_API int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto) {
    if (!cam) return -1;
    struct v4l2_control ctrl = {0};
    switch (param) {
        case WEBCAM_PARAM_EXPOSURE:
            ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
            ctrl.value = is_auto ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL;
            break;
        case WEBCAM_PARAM_FOCUS:
            ctrl.id    = V4L2_CID_FOCUS_AUTO;
            ctrl.value = is_auto ? 1 : 0;
            break;
        default:
            return -1;
    }
    return (ioctl(cam->fd, VIDIOC_S_CTRL, &ctrl) == 0) ? 0 : -1;
}

#endif /* __linux__ */
