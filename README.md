# Librería Webcam - Documentación

Librería multiplataforma (Linux/Windows) para captura de video desde webcams con **zero-copy** para máxima performance.

## 📋 Tabla de Contenidos

- [Características](#características)
- [Conceptos Básicos](#conceptos-básicos)
- [Ejemplos de Uso](#ejemplos-de-uso)
- [API Reference](#api-reference)
- [Formatos Soportados](#formatos-soportados)
- [Performance](#performance)

---

## Características

✅ **Zero-Copy**: Acceso directo al buffer de la cámara sin copias  
✅ **Query de Capacidades**: Descubre formatos y resoluciones soportadas  
✅ **Múltiples Formatos**: RGB24, RGB32, YUYV, YUV420, MJPEG  
✅ **Múltiples Buffers**: 4 buffers para evitar frame drops  
✅ **Controles**: Brillo, contraste, exposición, enfoque, zoom, etc.  
✅ **Multiplataforma**: Linux (V4L2) y Windows (Media Foundation)

---

## Conceptos Básicos

### Zero-Copy

El frame capturado **NO se copia** a memoria. El puntero `frame.data` apunta directamente al buffer mapeado de la cámara.

**Restricciones:**
- `frame.data` es **read-only**
- Válido solo hasta el próximo `capture()` o `release_frame()`
- Debes llamar `webcam_release_frame()` antes del siguiente frame

### Flujo de Trabajo

```
1. Listar dispositivos
2. Query de capacidades (opcional pero recomendado)
3. Seleccionar mejor formato
4. Abrir cámara
5. Loop:
   - Capturar frame (zero-copy)
   - Procesar frame.data
   - Liberar frame
6. Cerrar cámara
```

---

## Ejemplos de Uso

### Ejemplo 1: Captura Básica

```c
#include "webcam.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    // Abrir primera cámara en 640x480 YUYV
    Webcam* cam = webcam_open(640, 480, 0, WEBCAM_FMT_YUYV);
    if (!cam) {
        printf("Error abriendo cámara\n");
        return -1;
    }
    
    printf("Cámara abierta: %dx%d\n",
           webcam_get_actual_width(cam),
           webcam_get_actual_height(cam));
    
    // Capturar 10 frames
    WebcamFrame frame;
    for (int i = 0; i < 10; i++) {
        if (webcam_capture(cam, &frame) == 0) {
            printf("Frame %d: %d bytes\n", i, frame.size);
            
            // Procesar frame.data aquí...
            
            webcam_release_frame(cam);
        }
    }
    
    webcam_close(cam);
    return 0;
}
```

### Ejemplo 2: Listar Dispositivos

```c
#include "webcam.h"
#include <stdio.h>

int main() {
    int count;
    WebcamInfo* devices = webcam_list_devices(&count);
    
    printf("Cámaras encontradas: %d\n\n", count);
    
    for (int i = 0; i < count; i++) {
        printf("[%d] %s\n", devices[i].index, devices[i].name);
        printf("    Path: %s\n\n", devices[i].path);
    }
    
    webcam_free_list(devices);
    return 0;
}
```

### Ejemplo 3: Query de Capacidades

```c
#include "webcam.h"
#include <stdio.h>

int main() {
    WebcamCapabilities* caps = webcam_query_capabilities(0);
    if (!caps) {
        printf("Error obteniendo capacidades\n");
        return -1;
    }
    
    printf("Resoluciones soportadas:\n");
    printf("  Min: %dx%d\n", caps->min_width, caps->min_height);
    printf("  Max: %dx%d\n\n", caps->max_width, caps->max_height);
    
    printf("Formatos disponibles: %d\n\n", caps->format_count);
    
    const char* format_names[] = {"RGB24", "RGB32", "YUYV", "YUV420", "MJPEG"};
    
    for (int i = 0; i < caps->format_count; i++) {
        printf("  %4dx%4d @ %2d fps - %s\n",
               caps->formats[i].width,
               caps->formats[i].height,
               caps->formats[i].fps,
               format_names[caps->formats[i].format]);
    }
    
    webcam_free_capabilities(caps);
    return 0;
}
```

### Ejemplo 4: Controles de Cámara

```c
#include "webcam.h"
#include <stdio.h>

int main() {
    Webcam* cam = webcam_open(640, 480, 0, WEBCAM_FMT_YUYV);
    if (!cam) return -1;
    
    // Obtener valores actuales
    long brightness = webcam_get_parameter(cam, WEBCAM_PARAM_BRIGHTNESS);
    long contrast = webcam_get_parameter(cam, WEBCAM_PARAM_CONTRAST);
    
    printf("Brillo actual: %ld\n", brightness);
    printf("Contraste actual: %ld\n", contrast);
    
    // Ajustar brillo
    webcam_set_parameter(cam, WEBCAM_PARAM_BRIGHTNESS, 150);
    
    // Ajustar contraste
    webcam_set_parameter(cam, WEBCAM_PARAM_CONTRAST, 100);
    
    // Activar exposición automática
    webcam_set_auto(cam, WEBCAM_PARAM_EXPOSURE, 1);
    
    // Desactivar enfoque automático y fijar valor
    webcam_set_auto(cam, WEBCAM_PARAM_FOCUS, 0);
    webcam_set_parameter(cam, WEBCAM_PARAM_FOCUS, 50);
    
    webcam_close(cam);
    return 0;
}
```

### Ejemplo 5: Guardar Frame como PPM

```c
#include "webcam.h"
#include <stdio.h>
#include <stdlib.h>

void save_yuyv_as_ppm(const char* filename, 
                      const unsigned char* yuyv, 
                      int width, int height) {
    FILE* f = fopen(filename, "wb");
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    
    // Convertir YUYV a RGB y guardar
    for (int i = 0; i < width * height * 2; i += 4) {
        int y0 = yuyv[i];
        int u = yuyv[i + 1] - 128;
        int y1 = yuyv[i + 2];
        int v = yuyv[i + 3] - 128;
        
        // Pixel 1
        int r = y0 + (1.402 * v);
        int g = y0 - (0.344 * u) - (0.714 * v);
        int b = y0 + (1.772 * u);
        fputc(r < 0 ? 0 : (r > 255 ? 255 : r), f);
        fputc(g < 0 ? 0 : (g > 255 ? 255 : g), f);
        fputc(b < 0 ? 0 : (b > 255 ? 255 : b), f);
        
        // Pixel 2
        r = y1 + (1.402 * v);
        g = y1 - (0.344 * u) - (0.714 * v);
        b = y1 + (1.772 * u);
        fputc(r < 0 ? 0 : (r > 255 ? 255 : r), f);
        fputc(g < 0 ? 0 : (g > 255 ? 255 : g), f);
        fputc(b < 0 ? 0 : (b > 255 ? 255 : b), f);
    }
    
    fclose(f);
}

int main() {
    Webcam* cam = webcam_open(640, 480, 0, WEBCAM_FMT_YUYV);
    if (!cam) return -1;
    
    WebcamFrame frame;
    if (webcam_capture(cam, &frame) == 0) {
        save_yuyv_as_ppm("frame.ppm", frame.data, 
                         frame.width, frame.height);
        printf("Frame guardado en frame.ppm\n");
        webcam_release_frame(cam);
    }
    
    webcam_close(cam);
    return 0;
}
```

### Ejemplo 6: Captura Continua con FPS

```c
#include "webcam.h"
#include <stdio.h>
#include <time.h>

int main() {
    Webcam* cam = webcam_open(640, 480, 0, WEBCAM_FMT_YUYV);
    if (!cam) return -1;
    
    WebcamFrame frame;
    int frame_count = 0;
    time_t start = time(NULL);
    
    printf("Capturando durante 10 segundos...\n");
    
    while (time(NULL) - start < 10) {
        if (webcam_capture(cam, &frame) == 0) {
            frame_count++;
            
            // Procesar frame aquí...
            
            webcam_release_frame(cam);
        }
    }
    
    double elapsed = difftime(time(NULL), start);
    printf("Frames capturados: %d\n", frame_count);
    printf("FPS promedio: %.2f\n", frame_count / elapsed);
    
    webcam_close(cam);
    return 0;
}
```

---

## API Reference

### Enumeración de Dispositivos

```c
WebcamInfo* webcam_list_devices(int *count);
```
Lista todos los dispositivos de video disponibles.

**Parámetros:**
- `count`: Puntero donde se guardará la cantidad de dispositivos

**Retorna:** Array de `WebcamInfo` o `NULL` si no hay dispositivos

**Ejemplo:**
```c
int count;
WebcamInfo* devices = webcam_list_devices(&count);
// Usar...
webcam_free_list(devices);
```

---

```c
void webcam_free_list(WebcamInfo *list);
```
Libera la memoria del array de dispositivos.

---

### Query de Capacidades

```c
WebcamCapabilities* webcam_query_capabilities(int device_index);
```
Obtiene todos los formatos y resoluciones soportados por un dispositivo.

**Parámetros:**
- `device_index`: Índice del dispositivo (0 para primera cámara)

**Retorna:** Estructura con capacidades o `NULL` en error

---

```c
void webcam_free_capabilities(WebcamCapabilities *caps);
```
Libera memoria de capacidades.

---

### Ciclo de Vida de Cámara

```c
Webcam* webcam_open(int width, int height, int device_index, 
                    WebcamPixelFormat format);
```
Abre y configura una cámara.

**Parámetros:**
- `width`: Ancho deseado
- `height`: Alto deseado  
- `device_index`: Índice del dispositivo
- `format`: Formato de pixel deseado

**Retorna:** Handle de cámara o `NULL` en error

**Nota:** La resolución real puede diferir. Usar `webcam_get_actual_width/height()` para verificar.

---

```c
int webcam_capture(Webcam *cam, WebcamFrame *frame);
```
Captura un frame (zero-copy).

**Parámetros:**
- `cam`: Handle de cámara
- `frame`: Estructura donde se guardará info del frame

**Retorna:** 
- `0`: Éxito
- `-1`: Error
- `-2`: Timeout

**IMPORTANTE:** `frame->data` apunta al buffer interno. Llamar `webcam_release_frame()` antes del próximo capture.

---

```c
void webcam_release_frame(Webcam *cam);
```
Libera el frame actual para que la cámara pueda reutilizar el buffer.

**Debe llamarse** después de procesar cada frame y antes del siguiente `webcam_capture()`.

---

```c
void webcam_close(Webcam *cam);
```
Cierra la cámara y libera recursos.

---

### Información

```c
int webcam_get_actual_width(Webcam *cam);
int webcam_get_actual_height(Webcam *cam);
WebcamPixelFormat webcam_get_format(Webcam *cam);
```
Obtiene información de la configuración actual de la cámara.

---

### Controles

```c
long webcam_get_parameter(Webcam *cam, WebcamParameter param);
```
Obtiene valor actual de un parámetro.

**Retorna:** Valor del parámetro o `-1` si no está soportado

---

```c
int webcam_set_parameter(Webcam *cam, WebcamParameter param, long value);
```
Establece valor de un parámetro.

**Retorna:** `0` éxito, `-1` error

---

```c
int webcam_set_auto(Webcam *cam, WebcamParameter param, int is_auto);
```
Activa/desactiva modo automático para un parámetro.

**Parámetros soportados en auto:**
- `WEBCAM_PARAM_EXPOSURE`
- `WEBCAM_PARAM_FOCUS`

---

## Formatos Soportados

| Formato | Enum | Bytes/Pixel | Descripción |
|---------|------|-------------|-------------|
| RGB24 | `WEBCAM_FMT_RGB24` | 3 | R, G, B |
| RGB32 | `WEBCAM_FMT_RGB32` | 4 | R, G, B, A |
| YUYV | `WEBCAM_FMT_YUYV` | 2 | Y₀, U, Y₁, V (packed) |
| YUV420 | `WEBCAM_FMT_YUV420` | 1.5 | Planar Y + U/4 + V/4 |
| MJPEG | `WEBCAM_FMT_MJPEG` | Variable | JPEG comprimido |

### ¿Cuál formato usar?

- **YUYV**: Más común, buen balance calidad/velocidad
- **RGB24/RGB32**: Para display directo o procesamiento RGB
- **YUV420**: Más eficiente para video encoding
- **MJPEG**: Para alta resolución con menor bandwidth

---

## Performance

### Tips de Optimización

1. **Usar formato nativo de la cámara**: Consultar con `webcam_query_capabilities()`
2. **Procesar in-place**: Evitar copiar `frame.data`
3. **YUYV vs RGB**: YUYV es más rápido si no necesitas RGB
4. **MJPEG para alta resolución**: Menor bandwidth USB

---

## Manejo de Errores

```c
Webcam* cam = webcam_open(640, 480, 0, WEBCAM_FMT_YUYV);
if (!cam) {
    fprintf(stderr, "Error: No se pudo abrir la cámara\n");
    // Verificar permisos, disponibilidad, formato soportado
    return -1;
}

WebcamFrame frame;
int ret = webcam_capture(cam, &frame);
if (ret == -1) {
    fprintf(stderr, "Error capturando frame\n");
} else if (ret == -2) {
    fprintf(stderr, "Timeout esperando frame\n");
}
```

