// ============================================================================
// webcam_common.c
// ============================================================================
#include "webcam.h"
#include <stdlib.h>

WEBCAM_API void webcam_free_list(WebcamInfo *list) {
    if (list) free(list);
}

WEBCAM_API void webcam_free_capabilities(WebcamCapabilities *caps) {
    if (caps) {
        if (caps->formats) free(caps->formats);
        free(caps);
    }
}

WEBCAM_API WebcamFormatInfo* webcam_find_best_format(
    WebcamCapabilities *caps,
    int preferred_width,
    int preferred_height,
    WebcamPixelFormat preferred_format) {
    
    if (!caps || !caps->formats || caps->format_count == 0) return NULL;
    
    WebcamFormatInfo *best = NULL;
    int best_score = -999999;
    
    for (int i = 0; i < caps->format_count; i++) {
        WebcamFormatInfo *fmt = &caps->formats[i];
        int score = 0;
        
        if (fmt->format == preferred_format) score += 10000;
        
        int width_diff = abs(fmt->width - preferred_width);
        int height_diff = abs(fmt->height - preferred_height);
        score -= (width_diff + height_diff);
        
        if (score > best_score) {
            best_score = score;
            best = fmt;
        }
    }
    
    return best;
}
