#include "webcam.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

WEBCAM_API void webcam_free_list(WebcamInfo *list) {
    if (list) free(list);
}