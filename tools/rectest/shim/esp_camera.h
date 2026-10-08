#pragma once
#include "Arduino.h"
#include <sys/time.h>
typedef struct { uint8_t* buf; size_t len; size_t width; size_t height; int format; struct timeval timestamp; } camera_fb_t;
struct sensor_t { int (*set_saturation)(sensor_t*, int); struct { int8_t saturation; } status; };
sensor_t* esp_camera_sensor_get();
camera_fb_t* esp_camera_fb_get();
void esp_camera_fb_return(camera_fb_t*);
