#include "webcam.h"
#include <stdio.h>
#include <stdlib.h>

void my_logger(const char* msg) {
    printf("[LIB LOG]: %s\n", msg);
}

int main() {
    webcam_set_logger(my_logger);
    
    int count = 0;
    WebcamInfo* list = webcam_list_devices(&count);
    printf("Camaras encontradas: %d\n", count);
    
    if (count == 0) return 1;
    
    for(int i=0; i<count; i++) printf("ID: %d - %s\n", list[i].index, list[i].name);
    
    int idx = list[0].index;
    webcam_free_list(list);

    // Intentar abrir
    Webcam *cam = webcam_open(640, 480, idx, WEBCAM_FMT_RGB24);
    if (!cam) return 1;
    
    printf("Camara abierta. Resolucion real: %dx%d\n", 
            webcam_get_actual_width(cam), webcam_get_actual_height(cam));

    WebcamFrame frame;
    frame.data = malloc(1920 * 1080 * 3); // Reserva suficiente

    for(int i=0; i<10; i++) {
        int res = webcam_capture(cam, &frame);
        if (res == 0) {
            printf("Frame %d OK. Size: %d bytes. Time: %lu ms\n", i, frame.size, frame.timestamp_ms);
        } else {
            printf("Error captura: %d\n", res);
        }
    }

    free(frame.data);
    webcam_close(cam);
    return 0;
}