#ifndef _SYS_REDWOOD_CAMERA_H_
#define _SYS_REDWOOD_CAMERA_H_
#include <stdint.h>
#define REDWOOD_CAMERA_DEVICE "/dev/redwood-camera"
#define REDWOOD_CAMERA_MAGIC "RWFRAME1"
struct redwood_camera_frame {
 char magic[8];
 uint32_t header_bytes;
 uint32_t width, height;
 uint32_t stride;
 uint32_t format;
 uint32_t bayer;
 uint32_t black;
 uint64_t sequence;
 uint64_t time;
 uint32_t exposure_lines;
 uint32_t line_ns;
 uint32_t gain;
 uint32_t gain_unity;
 uint32_t gain_max;
 uint32_t frame_lines;
 uint32_t frame_lines_min;
 uint32_t frame_lines_max;
 uint32_t margin;
 uint32_t sensor;
 uint32_t focus;
 uint32_t reserved[5];
};
#define REDWOOD_CAMERA_NO_FOCUS 0xffffffffu
enum {
 REDWOOD_CAMERA_EXPOSURE = 1,
 REDWOOD_CAMERA_KEEP = 2,
 REDWOOD_CAMERA_SENSOR = 3,
 REDWOOD_CAMERA_FOCUS = 4,
};
struct redwood_camera_command {
 uint32_t command;
 uint32_t bytes;
 uint32_t a, b, c, d;
};
#endif
