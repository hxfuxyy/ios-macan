#ifndef CAMERA_ISP_H
#define CAMERA_ISP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "redwood_camera.h"
struct isp_stats {
 double mean[3];
 double bright;
 uint32_t hist[64];
 uint32_t cells;
 double grey[2], neutral[2];
 uint32_t grey_cells, neutral_cells;
};
#define LSC_W 17
#define LSC_H 13
struct isp_lsc {
 bool on;
 float gain[3][LSC_H][LSC_W];
};
#define TUNE_LIGHTS 6
#define TUNE_CCTS 6
struct isp_tune_set {
 uint32_t lights;
 struct {
  float lux_from, lux_to;
  uint32_t ccts;
  float cct_from[TUNE_CCTS], cct_to[TUNE_CCTS];
  const float *v[TUNE_CCTS];
 } light[TUNE_LIGHTS];
};
struct isp_tuning {
 struct isp_tune_set ccm, lsc;
 uint32_t refs;
 float ref[16][3];
 void *file;
};
struct isp {
 float wb[3];
 float digital;
 float zoom;
 bool turned;
 float ccm[3][3];
 struct isp_lsc lsc;
 uint8_t gamma[4096];
 uint32_t asked_lines, asked_gain, asked_frame_lines;
 bool asked;
 uint32_t max_frame_lines;
 struct isp_tuning *tuning;
};
void isp_init(struct isp *isp);
float isp_sharpness(const struct redwood_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level);
void isp_measure(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, struct isp_stats *s);
bool isp_ae(struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *s, struct redwood_camera_command *c);
void isp_awb(struct isp *isp, const struct isp_stats *s);
void isp_render_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range);
void isp_render_bgra(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row);
void isp_render_still_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y,
    size_t y_stride, uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row,
    bool video_range);
void isp_render_full_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range);
bool isp_lsc_from_flat(struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, float *min_ratio);
void isp_set_ccm(struct isp *isp, const float m[9]);
bool isp_load_tuning(struct isp *isp, const char *path);
void isp_tune(struct isp *isp, float lux_index, float cct);
float isp_cct(const struct isp *isp);
#endif
