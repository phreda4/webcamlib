#include "webcam.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

static WebcamLogFunc g_log_func = NULL;

WEBCAM_API void webcam_set_logger(WebcamLogFunc func) {
    g_log_func = func;
}

// Funcion interna para logs
void webcam_log(const char *format, ...) {
    if (g_log_func) {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        g_log_func(buffer);
    }
}

WEBCAM_API void webcam_free_list(WebcamInfo *list) {
    if (list) free(list);
}