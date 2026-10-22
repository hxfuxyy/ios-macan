#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "camera_isp.h"
#define WHITE 959
#define LUT_WHITE 3836
void
isp_init(struct isp *isp)
{
 memset(isp, 0, sizeof(*isp));
 isp->wb[0] = isp->wb[1] = isp->wb[2] = 1;
 isp->digital = 1;
 isp->zoom = 1;
 for (int i = 0; i < 3; i++) {
  isp->ccm[i][i] = 1;
 }
 for (int i = 0; i < 4096; i++) {
  double v = i >= LUT_WHITE ? 1 : (double)i / LUT_WHITE;
  v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
  isp->gamma[i] = (uint8_t)(v * 255 + 0.5);
 }
}
static void
cell_places(uint32_t bayer, int *r, int *b)
{
 static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };
 *r = red[bayer & 3];
 *b = blue[bayer & 3];
}
static inline void
cell(const uint8_t *top, size_t stride, uint32_t cx, int v[4])
{
 const uint8_t *g0 = top + (size_t)(cx >> 1) * 5, *g1 = g0 + stride;
 unsigned o = (cx & 1) * 2;
 v[0] = (g0[o] << 2 | (g0[4] >> (o * 2) & 3)) - 64;
 v[1] = (g0[o + 1] << 2 | (g0[4] >> (o * 2 + 2) & 3)) - 64;
 v[2] = (g1[o] << 2 | (g1[4] >> (o * 2) & 3)) - 64;
 v[3] = (g1[o + 1] << 2 | (g1[4] >> (o * 2 + 2) & 3)) - 64;
 for (int i = 0; i < 4; i++) {
  v[i] = v[i] < 0 ? 0 : v[i];
 }
}
float
isp_sharpness(const struct redwood_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level)
{
 enum { BLOCK = 4, MAX_BLOCKS = 512 };
 uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
 uint32_t cx0 = (uint32_t)(x0 * cells_w), cx1 = (uint32_t)(x1 * cells_w), cy0 = (uint32_t)(y0 * cells_h),
     cy1 = (uint32_t)(y1 * cells_h);
 float above[MAX_BLOCKS], here[MAX_BLOCKS];
 double steps = 0, sum = 0;
 uint32_t blocks_w, rows = 0, n = 0;
 int r, b;
 cell_places(f->bayer, &r, &b);
 cx1 = cx1 > cells_w ? cells_w : cx1;
 cy1 = cy1 > cells_h ? cells_h : cy1;
 blocks_w = cx1 > cx0 ? (cx1 - cx0) / BLOCK : 0;
 blocks_w = blocks_w > MAX_BLOCKS ? MAX_BLOCKS : blocks_w;
 for (uint32_t by = cy0; by + BLOCK <= cy1; by += BLOCK, rows++) {
  for (uint32_t bx = 0; bx < blocks_w; bx++) {
   uint32_t g = 0;
   for (uint32_t cy = by; cy < by + BLOCK; cy++) {
    const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
    for (uint32_t cx = cx0 + bx * BLOCK; cx < cx0 + bx * BLOCK + BLOCK; cx++) {
     int v[4];
     cell(top, f->stride, cx, v);
     for (int i = 0; i < 4; i++) {
      g += i != r && i != b ? (uint32_t)v[i] : 0;
     }
    }
   }
   here[bx] = g / (2.0f * BLOCK * BLOCK);
   sum += here[bx];
   n++;
   if (bx > 0) {
    steps += (here[bx] - here[bx - 1]) * (here[bx] - here[bx - 1]);
   }
   if (rows > 0) {
    steps += (here[bx] - above[bx]) * (here[bx] - above[bx]);
   }
  }
  memcpy(above, here, sizeof(float) * blocks_w);
 }
 double mean = n ? sum / n : 0;
 if (level != NULL) {
  *level = (float)mean;
 }
 return mean > 0 ? (float)(steps / n / (mean * mean)) : 0;
}
static inline void lsc_at(const struct isp *isp, float u, float v, float g[3]);
void
isp_measure(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, struct isp_stats *s)
{
 uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
 double sum[3] = { 0, 0, 0 }, grey[2] = { 0, 0 }, neutral[2] = { 0, 0 };
 float gains[3] = { 1, 1, 1 };
 int r, b;
 memset(s, 0, sizeof(*s));
 cell_places(f->bayer, &r, &b);
 for (uint32_t cy = 0; cy < cells_h; cy += 4) {
  const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
  float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0;
  for (uint32_t cx = 0; cx < cells_w; cx += 4) {
   int v[4], g = 0, top_v = 0;
   cell(top, f->stride, cx, v);
   for (int i = 0; i < 4; i++) {
    g += i != r && i != b ? v[i] : 0;
    top_v = v[i] > top_v ? v[i] : top_v;
   }
   g /= 2;
   sum[0] += v[r];
   sum[1] += g;
   sum[2] += v[b];
   if (top_v < WHITE * 9 / 10 && g >= 24) {
    float cr = (float)v[r], cg = (float)g, cb = (float)v[b];
    if (isp != NULL && isp->lsc.on) {
     lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
     cr *= gains[0];
     cg *= gains[1];
     cb *= gains[2];
    }
    grey[0] += cr / cg;
    grey[1] += cb / cg;
    s->grey_cells++;
    if (isp != NULL) {
     float wr = cr * isp->wb[0] / cg, wb = cb * isp->wb[2] / cg;
     if (wr > 0.75f && wr < 1.33f && wb > 0.75f && wb < 1.33f) {
      neutral[0] += cr / cg;
      neutral[1] += cb / cg;
      s->neutral_cells++;
     }
    }
   }
   s->hist[g >> 4 > 63 ? 63 : g >> 4]++;
   s->cells++;
  }
 }
 for (int c = 0; c < 3; c++) {
  s->mean[c] = s->cells ? sum[c] / s->cells : 0;
 }
 for (int c = 0; c < 2; c++) {
  s->grey[c] = s->grey_cells ? grey[c] / s->grey_cells : 0;
  s->neutral[c] = s->neutral_cells ? neutral[c] / s->neutral_cells : 0;
 }
 for (int i = 63, above = 0; i >= 0; i--) {
  above += s->hist[i];
  if (above * 100 >= (int)s->cells) {
   s->bright = i * 16 + 8;
   break;
  }
 }
}
bool
isp_ae(struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *s, struct redwood_camera_command *c)
{
 double now = (double)f->exposure_lines * f->gain / f->gain_unity * isp->digital, ratio, want;
 double target = 0.15 * WHITE, mean = s->mean[1] * isp->digital > 0.5 ? s->mean[1] * isp->digital : 0.5;
 double bright = s->bright * isp->digital;
 uint32_t fast = f->frame_lines_min - f->margin, longest = isp->max_frame_lines ? isp->max_frame_lines : 2 * f->frame_lines_min;
 uint32_t lines, gain, frame_lines = 0;
 double digital = 1;
 if (isp->asked && (f->exposure_lines != isp->asked_lines || f->gain != isp->asked_gain)) {
  return false;
 }
 ratio = target / mean;
 if (bright * ratio > 0.9 * WHITE && bright > 0) {
  ratio = 0.9 * WHITE / bright > target / mean * 0.5 ? 0.9 * WHITE / bright : target / mean * 0.5;
 }
 ratio = ratio < 0.25 ? 0.25 : ratio > 4 ? 4 : ratio;
 if (ratio > 0.94 && ratio < 1.06) {
  return false;
 }
 want = now * pow(ratio, 0.7);
 if (want <= fast) {
  lines = want < 4 ? 4 : (uint32_t)want;
  gain = f->gain_unity;
 } else if (want <= (double)fast * 16) {
  lines = fast;
  gain = (uint32_t)(want / fast * f->gain_unity);
 } else {
  lines = (uint32_t)(want / 16);
  lines = lines > longest - f->margin ? longest - f->margin : lines < fast ? fast : lines;
  frame_lines = lines + f->margin;
  double g = want / lines;
  if (g > (double)f->gain_max / f->gain_unity) {
   digital = g / ((double)f->gain_max / f->gain_unity);
   digital = digital > 4 ? 4 : digital;
   g = (double)f->gain_max / f->gain_unity;
  }
  gain = (uint32_t)(g * f->gain_unity);
 }
 isp->digital = (float)digital;
 isp->asked_lines = lines;
 isp->asked_gain = gain;
 isp->asked_frame_lines = frame_lines;
 isp->asked = true;
 memset(c, 0, sizeof(*c));
 c->command = REDWOOD_CAMERA_EXPOSURE;
 c->a = lines;
 c->b = gain;
 c->c = frame_lines;
 return true;
}
static void
on_reference_line(const struct isp_tuning *t, float rg, float bg, uint32_t *segment, float *along)
{
 float x = logf(rg), y = logf(bg), best = INFINITY;
 *segment = 0;
 *along = 0;
 for (uint32_t i = 0; i + 1 < t->refs; i++) {
  float x0 = logf(t->ref[i][0]), y0 = logf(t->ref[i][1]), dx = logf(t->ref[i + 1][0]) - x0,
      dy = logf(t->ref[i + 1][1]) - y0, length = dx * dx + dy * dy;
  float u = length > 0 ? ((x - x0) * dx + (y - y0) * dy) / length : 0, d;
  u = u < 0 ? 0 : u > 1 ? 1 : u;
  d = (x0 + u * dx - x) * (x0 + u * dx - x) + (y0 + u * dy - y) * (y0 + u * dy - y);
  if (d < best) {
   best = d;
   *segment = i;
   *along = u;
  }
 }
}
float
isp_cct(const struct isp *isp)
{
 const struct isp_tuning *t = isp->tuning;
 uint32_t i;
 float u;
 if (t == NULL || t->refs < 2 || isp->wb[0] <= 0 || isp->wb[2] <= 0) {
  return 0;
 }
 on_reference_line(t, 1 / isp->wb[0], 1 / isp->wb[2], &i, &u);
 return t->ref[i][2] + u * (t->ref[i + 1][2] - t->ref[i][2]);
}
void
isp_awb(struct isp *isp, const struct isp_stats *s)
{
 float r, b;
 if (s->neutral_cells * 32 >= s->cells && s->neutral[0] > 0.01 && s->neutral[1] > 0.01) {
  r = (float)(1 / s->neutral[0]);
  b = (float)(1 / s->neutral[1]);
 } else if (s->grey_cells * 16 >= s->cells && s->grey[0] > 0.01 && s->grey[1] > 0.01) {
  r = (float)(1 / s->grey[0]);
  b = (float)(1 / s->grey[1]);
 } else if (s->mean[0] >= 2 && s->mean[1] >= 2 && s->mean[2] >= 2) {
  r = (float)(s->mean[1] / s->mean[0]);
  b = (float)(s->mean[1] / s->mean[2]);
 } else {
  return;
 }
 if (isp->tuning != NULL && isp->tuning->refs >= 2) {
  const struct isp_tuning *t = isp->tuning;
  uint32_t i;
  float u;
  on_reference_line(t, 1 / r, 1 / b, &i, &u);
  r = 1 / expf(logf(t->ref[i][0]) + u * (logf(t->ref[i + 1][0]) - logf(t->ref[i][0])));
  b = 1 / expf(logf(t->ref[i][1]) + u * (logf(t->ref[i + 1][1]) - logf(t->ref[i][1])));
 }
 r = r < 0.5f ? 0.5f : r > 4 ? 4 : r;
 b = b < 0.5f ? 0.5f : b > 4 ? 4 : b;
 isp->wb[0] = isp->wb[0] * 0.9f + r * 0.1f;
 isp->wb[2] = isp->wb[2] * 0.9f + b * 0.1f;
}
static void
crop(const struct isp *isp, const struct redwood_camera_frame *f, uint32_t width, uint32_t height, uint32_t *x0, uint32_t *y0,
    uint32_t *cw, uint32_t *ch)
{
 uint32_t w = f->width / 2, h = f->height / 2;
 float zoom = isp->zoom < 1 ? 1 : isp->zoom;
 if ((uint64_t)width * h > (uint64_t)height * w) {
  *cw = w;
  *ch = (uint32_t)((uint64_t)w * height / width);
 } else {
  *ch = h;
  *cw = (uint32_t)((uint64_t)h * width / height);
 }
 *cw = (uint32_t)(*cw / zoom) < 2 ? 2 : (uint32_t)(*cw / zoom);
 *ch = (uint32_t)(*ch / zoom) < 2 ? 2 : (uint32_t)(*ch / zoom);
 *x0 = (w - *cw) / 2;
 *y0 = (h - *ch) / 2;
}
void
isp_set_ccm(struct isp *isp, const float m[9])
{
 for (int i = 0; i < 9; i++) {
  isp->ccm[i / 3][i % 3] = m[i];
 }
}
struct pipe {
 float m[3][3];
 bool identity;
};
static void
pipe_init(const struct isp *isp, struct pipe *p)
{
 p->identity = true;
 for (int c = 0; c < 3; c++) {
  for (int j = 0; j < 3; j++) {
   p->m[c][j] = isp->ccm[c][j] * isp->wb[j] * isp->digital * (LUT_WHITE / (float)WHITE);
   p->identity = p->identity && (c == j ? isp->ccm[c][j] == 1 : isp->ccm[c][j] == 0);
  }
 }
}
static inline uint8_t
curve(const struct isp *isp, float v)
{
 int i = (int)(v + 0.5f);
 return isp->gamma[i < 0 ? 0 : i > 4095 ? 4095 : i];
}
static inline void
pixel(const struct isp *isp, const struct pipe *p, float r, float g, float b, uint8_t *rgb)
{
 if (p->identity) {
  rgb[0] = curve(isp, r * p->m[0][0]);
  rgb[1] = curve(isp, g * p->m[1][1]);
  rgb[2] = curve(isp, b * p->m[2][2]);
  return;
 }
 rgb[0] = curve(isp, p->m[0][0] * r + p->m[0][1] * g + p->m[0][2] * b);
 rgb[1] = curve(isp, p->m[1][0] * r + p->m[1][1] * g + p->m[1][2] * b);
 rgb[2] = curve(isp, p->m[2][0] * r + p->m[2][1] * g + p->m[2][2] * b);
}
static inline void
lsc_at(const struct isp *isp, float u, float v, float g[3])
{
 float gx = u * (LSC_W - 1), gy = v * (LSC_H - 1);
 int x = (int)gx, y = (int)gy;
 x = x < 0 ? 0 : x > LSC_W - 2 ? LSC_W - 2 : x;
 y = y < 0 ? 0 : y > LSC_H - 2 ? LSC_H - 2 : y;
 float fx = gx - x, fy = gy - y;
 for (int c = 0; c < 3; c++) {
  const float (*t)[LSC_W] = isp->lsc.gain[c];
  g[c] = (t[y][x] * (1 - fx) + t[y][x + 1] * fx) * (1 - fy) + (t[y + 1][x] * (1 - fx) + t[y + 1][x + 1] * fx) * fy;
 }
}
static void
render_line(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint32_t width, uint32_t height,
    uint32_t row, uint8_t *rgb)
{
 uint32_t x0, y0, cw, ch, cells_w = f->width / 2, cells_h = f->height / 2;
 int r, b;
 struct pipe p;
 crop(isp, f, width, height, &x0, &y0, &cw, &ch);
 cell_places(f->bayer, &r, &b);
 pipe_init(isp, &p);
 if (isp->turned) {
  row = height - 1 - row;
 }
 if (cw < width && ch < height) {
  float fy = y0 + (row + 0.5f) * ch / height - 0.5f, ty, v_at;
  uint32_t cy0 = fy < 0 ? 0 : (uint32_t)fy, cy1;
  cy0 = cy0 > cells_h - 1 ? cells_h - 1 : cy0;
  cy1 = cy0 + 1 > cells_h - 1 ? cells_h - 1 : cy0 + 1;
  ty = fy < (float)cy0 ? 0 : fy - cy0;
  v_at = cells_h > 1 ? (float)cy0 / (cells_h - 1) : 0;
  const uint8_t *t0 = raw + (size_t)cy0 * 2 * f->stride, *t1 = raw + (size_t)cy1 * 2 * f->stride;
  float gains[3] = { 1, 1, 1 };
  for (uint32_t x = 0; x < width; x++) {
   float fx = x0 + (x + 0.5f) * cw / width - 0.5f, tx, c[3] = { 0, 0, 0 };
   uint32_t cx0 = fx < 0 ? 0 : (uint32_t)fx, cx1;
   cx0 = cx0 > cells_w - 1 ? cells_w - 1 : cx0;
   cx1 = cx0 + 1 > cells_w - 1 ? cells_w - 1 : cx0 + 1;
   tx = fx < (float)cx0 ? 0 : fx - cx0;
   for (int k = 0; k < 4; k++) {
    int v[4];
    float w = (k & 1 ? tx : 1 - tx) * (k & 2 ? ty : 1 - ty);
    cell(k & 2 ? t1 : t0, f->stride, k & 1 ? cx1 : cx0, v);
    c[0] += w * v[r];
    c[2] += w * v[b];
    for (int j = 0; j < 4; j++) {
     c[1] += j != r && j != b ? w * v[j] * 0.5f : 0;
    }
   }
   if (isp->lsc.on) {
    lsc_at(isp, cells_w > 1 ? (float)cx0 / (cells_w - 1) : 0, v_at, gains);
   }
   pixel(isp, &p, c[0] * gains[0], c[1] * gains[1], c[2] * gains[2], rgb + 3 * (isp->turned ? width - 1 - x : x));
  }
  return;
 }
 uint32_t cy = y0 + (uint32_t)((uint64_t)row * ch / height);
 const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
 uint64_t step = ((uint64_t)cw << 16) / width, at = (uint64_t)x0 << 16;
 float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0, gains[3] = { 1, 1, 1 };
 for (uint32_t x = 0; x < width; x++, at += step) {
  uint32_t cx = (uint32_t)(at >> 16);
  int v[4], g = 0;
  cell(top, f->stride, cx, v);
  for (int j = 0; j < 4; j++) {
   g += j != r && j != b ? v[j] : 0;
  }
  if (isp->lsc.on) {
   lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
  }
  pixel(isp, &p, v[r] * gains[0], g * 0.5f * gains[1], v[b] * gains[2], rgb + 3 * (isp->turned ? width - 1 - x : x));
 }
}
static void
nv12_pair(const uint8_t *rgb0, const uint8_t *rgb1, uint32_t width, uint8_t *y0, uint8_t *y1, uint8_t *uv, bool video_range)
{
 static const int32_t full[9] = { 19595, 38470, 7471, -11058, -21710, 32768, 32768, -27439, -5329 };
 static const int32_t video[9] = { 16829, 33039, 6416, -9714, -19070, 28784, 28784, -24103, -4681 };
 const int32_t *k = video_range ? video : full;
 int32_t oy = (video_range ? 16 << 16 : 0) + (1 << 15);
 for (int l = 0; l < 2; l++) {
  const uint8_t *p = l ? rgb1 : rgb0;
  uint8_t *out = l ? y1 : y0;
  for (uint32_t x = 0; out != NULL && x < width; x++, p += 3) {
   out[x] = (uint8_t)((k[0] * p[0] + k[1] * p[1] + k[2] * p[2] + oy) >> 16);
  }
 }
 for (uint32_t x = 0; x < width; x += 2) {
  uint32_t x1 = x + 1 < width ? x + 1 : x;
  int32_t R = rgb0[3 * x] + rgb0[3 * x1] + rgb1[3 * x] + rgb1[3 * x1];
  int32_t G = rgb0[3 * x + 1] + rgb0[3 * x1 + 1] + rgb1[3 * x + 1] + rgb1[3 * x1 + 1];
  int32_t B = rgb0[3 * x + 2] + rgb0[3 * x1 + 2] + rgb1[3 * x + 2] + rgb1[3 * x1 + 2];
  int32_t cb = (k[3] * R + k[4] * G + k[5] * B + (128 << 18) + (1 << 17) - 1) >> 18;
  int32_t cr = (k[6] * R + k[7] * G + k[8] * B + (128 << 18) + (1 << 17) - 1) >> 18;
  uv[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
  uv[x + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
 }
}
void
isp_render_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
 uint8_t line[2][3 * 8192];
 if (width > 8192) {
  return;
 }
 for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
  bool second = row + 1 < height;
  render_line(isp, f, raw, width, height, row, line[0]);
  render_line(isp, f, raw, width, height, second ? row + 1 : row, line[1]);
  nv12_pair(line[0], line[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
      uv + (size_t)(row / 2) * uv_stride, video_range);
 }
}
void
isp_render_bgra(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
 uint8_t line[3 * 8192];
 if (width > 8192) {
  return;
 }
 for (uint32_t row = first_row; row < end_row && row < height; row++) {
  uint8_t *o = out + (size_t)row * stride;
  render_line(isp, f, raw, width, height, row, line);
  for (uint32_t x = 0; x < width; x++) {
   o[4 * x] = line[3 * x + 2];
   o[4 * x + 1] = line[3 * x + 1];
   o[4 * x + 2] = line[3 * x];
   o[4 * x + 3] = 255;
  }
 }
}
static void
cfa(uint32_t bayer, int colour[4])
{
 int r, b;
 cell_places(bayer, &r, &b);
 for (int i = 0; i < 4; i++) {
  colour[i] = i == r ? 0 : i == b ? 2 : 1;
 }
}
static void
unpack_line(const struct redwood_camera_frame *f, const uint8_t *raw, int y, int *out)
{
 y = y < 0 ? 0 : y >= (int)f->height ? (int)f->height - 1 : y;
 const uint8_t *line = raw + (size_t)y * f->stride;
 for (uint32_t x = 0; x + 3 < f->width + 3 && x < f->width; x += 4) {
  const uint8_t *g = line + (x >> 2) * 5;
  for (int i = 0; i < 4 && x + i < f->width; i++) {
   int v = (g[i] << 2 | (g[4] >> (i * 2) & 3)) - (int)f->black;
   out[x + i] = v < 0 ? 0 : v;
  }
 }
}
static void
full_line(const struct isp *isp, const struct redwood_camera_frame *f, const int *const l[3], uint32_t y, const int colour[4],
    const struct pipe *p, uint8_t *rgb)
{
 int w = (int)f->width;
 float v_pos = f->height > 1 ? (float)y / (f->height - 1) : 0, gains[3] = { 1, 1, 1 };
 for (int x = 0; x < w; x++) {
  float sum[3] = { 0, 0, 0 };
  int n[3] = { 0, 0, 0 };
  for (int dy = -1; dy <= 1; dy++) {
   const int *line = l[dy + 1];
   int yy = (int)y + dy;
   yy = yy < 0 ? 0 : yy >= (int)f->height ? (int)f->height - 1 : yy;
   for (int dx = -1; dx <= 1; dx++) {
    int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
    int c = colour[(yy & 1) * 2 + (xx & 1)];
    if (dx == 0 && dy == 0) {
     sum[c] = (float)line[xx] * 64;
     n[c] = 64;
    } else if (n[c] < 64) {
     sum[c] += (float)line[xx];
     n[c]++;
    }
   }
  }
  if (isp->lsc.on) {
   lsc_at(isp, w > 1 ? (float)x / (w - 1) : 0, v_pos, gains);
  }
  pixel(isp, p, sum[0] / (n[0] ? n[0] : 1) * gains[0], sum[1] / (n[1] ? n[1] : 1) * gains[1],
      sum[2] / (n[2] ? n[2] : 1) * gains[2], rgb + 3 * x);
 }
}
void
isp_render_full_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range)
{
 uint32_t width = f->width;
 int colour[4];
 struct pipe p;
 static __thread int *lines[4];
 static __thread uint8_t *rgb[2];
 static __thread uint32_t have;
 if (width > have) {
  for (int i = 0; i < 4; i++) {
   free(lines[i]);
   lines[i] = malloc(sizeof(int) * width);
  }
  for (int i = 0; i < 2; i++) {
   free(rgb[i]);
   rgb[i] = malloc(3 * (size_t)width);
  }
  have = width;
 }
 cfa(f->bayer, colour);
 pipe_init(isp, &p);
 for (uint32_t row = first_row & ~1u; row < end_row && row < f->height; row += 2) {
  for (int i = 0; i < 4; i++) {
   unpack_line(f, raw, (int)row - 1 + i, lines[i]);
  }
  const int *const above[3] = { lines[0], lines[1], lines[2] }, *const below[3] = { lines[1], lines[2], lines[3] };
  bool second = row + 1 < f->height;
  full_line(isp, f, above, row, colour, &p, rgb[0]);
  full_line(isp, f, below, second ? row + 1 : row, colour, &p, rgb[1]);
  nv12_pair(rgb[0], rgb[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
      uv + (size_t)(row / 2) * uv_stride, video_range);
 }
}
static void
demosaic_row(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint32_t row, const int colour[4],
    const struct pipe *p, int *const l[3], uint8_t *rgb)
{
 for (int i = 0; i < 3; i++) {
  unpack_line(f, raw, (int)row - 1 + i, l[i]);
 }
 full_line(isp, f, (const int *const *)l, row, colour, p, rgb);
}
void
isp_render_still_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
 uint32_t x0, y0, cw, ch, fw = f->width;
 int colour[4];
 struct pipe p;
 static __thread int *lines[3];
 static __thread uint8_t *rows[2], *out[2];
 static __thread uint32_t have_fw, have_width;
 int32_t cached[2] = { -1, -1 };
 if (fw > have_fw) {
  for (int i = 0; i < 3; i++) {
   free(lines[i]);
   lines[i] = malloc(sizeof(int) * fw);
  }
  for (int i = 0; i < 2; i++) {
   free(rows[i]);
   rows[i] = malloc(3 * (size_t)fw);
  }
  have_fw = fw;
 }
 if (width > have_width) {
  for (int i = 0; i < 2; i++) {
   free(out[i]);
   out[i] = malloc(3 * (size_t)width);
  }
  have_width = width;
 }
 if (lines[2] == NULL || rows[1] == NULL || out[1] == NULL) {
  return;
 }
 crop(isp, f, width, height, &x0, &y0, &cw, &ch);
 cfa(f->bayer, colour);
 pipe_init(isp, &p);
 float sx_step = 2.0f * cw / width, sy_step = 2.0f * ch / height;
 for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
  for (int k = 0; k < 2; k++) {
   uint32_t r = row + k < height ? row + k : row, at = isp->turned ? height - 1 - r : r;
   float sy = 2.0f * y0 + (at + 0.5f) * sy_step - 0.5f, ty;
   uint32_t sy0 = sy < 0 ? 0 : (uint32_t)sy, sy1, slot[2];
   sy0 = sy0 > f->height - 1 ? f->height - 1 : sy0;
   sy1 = sy0 + 1 > f->height - 1 ? f->height - 1 : sy0 + 1;
   ty = sy < (float)sy0 ? 0 : sy - sy0;
   for (int n = 0; n < 2; n++) {
    uint32_t want = n ? sy1 : sy0;
    if (cached[0] == (int32_t)want || cached[1] == (int32_t)want) {
     slot[n] = cached[0] == (int32_t)want ? 0 : 1;
     continue;
    }
    slot[n] = n == 1 ? slot[0] ^ 1 : cached[0] == (int32_t)sy1 ? 1 : 0;
    demosaic_row(isp, f, raw, want, colour, &p, lines, rows[slot[n]]);
    cached[slot[n]] = (int32_t)want;
   }
   const uint8_t *a = rows[slot[0]], *b = rows[slot[1]];
   uint8_t *o = out[k];
   for (uint32_t x = 0; x < width; x++) {
    float sx = 2.0f * x0 + (x + 0.5f) * sx_step - 0.5f, tx;
    uint32_t sx0 = sx < 0 ? 0 : (uint32_t)sx, sx1;
    sx0 = sx0 > fw - 1 ? fw - 1 : sx0;
    sx1 = sx0 + 1 > fw - 1 ? fw - 1 : sx0 + 1;
    tx = sx < (float)sx0 ? 0 : sx - sx0;
    uint8_t *px = o + 3 * (isp->turned ? width - 1 - x : x);
    for (int c = 0; c < 3; c++) {
     float top = a[3 * sx0 + c] + tx * (a[3 * sx1 + c] - a[3 * sx0 + c]);
     float bottom = b[3 * sx0 + c] + tx * (b[3 * sx1 + c] - b[3 * sx0 + c]);
     px[c] = (uint8_t)(top + ty * (bottom - top) + 0.5f);
    }
   }
  }
  bool second = row + 1 < height;
  nv12_pair(out[0], out[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
      uv + (size_t)(row / 2) * uv_stride, video_range);
 }
}
bool
isp_lsc_from_flat(struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, float *min_ratio)
{
 uint32_t cells_w = f->width / 2, cells_h = f->height / 2, half_w = cells_w / (LSC_W - 1) / 2, half_h = cells_h / (LSC_H - 1) / 2;
 float level[3][LSC_H][LSC_W], top[3] = { 0, 0, 0 };
 uint32_t clipped = 0, seen = 0;
 int r, b;
 cell_places(f->bayer, &r, &b);
 for (int gy = 0; gy < LSC_H; gy++) {
  for (int gx = 0; gx < LSC_W; gx++) {
   int cx0 = (int)((uint64_t)gx * (cells_w - 1) / (LSC_W - 1)) - (int)half_w;
   int cy0 = (int)((uint64_t)gy * (cells_h - 1) / (LSC_H - 1)) - (int)half_h;
   double sum[3] = { 0, 0, 0 };
   uint32_t n = 0;
   for (int cy = cy0; cy <= cy0 + 2 * (int)half_h; cy += 2) {
    for (int cx = cx0; cx <= cx0 + 2 * (int)half_w; cx += 2) {
     int v[4];
     if (cx < 0 || cy < 0 || cx >= (int)cells_w || cy >= (int)cells_h) {
      continue;
     }
     cell(raw + (size_t)cy * 2 * f->stride, f->stride, (uint32_t)cx, v);
     sum[0] += v[r];
     sum[2] += v[b];
     for (int j = 0; j < 4; j++) {
      sum[1] += j != r && j != b ? v[j] / 2.0 : 0;
      clipped += v[j] > WHITE - 30;
     }
     seen += 4;
     n++;
    }
   }
   for (int c = 0; c < 3; c++) {
    level[c][gy][gx] = n ? (float)(sum[c] / n) : 0;
    top[c] = level[c][gy][gx] > top[c] ? level[c][gy][gx] : top[c];
   }
  }
 }
 if (top[1] < 100 || clipped * 1000 > seen) {
  return false;
 }
 float darkest = 1;
 for (int c = 0; c < 3; c++) {
  for (int gy = 0; gy < LSC_H; gy++) {
   for (int gx = 0; gx < LSC_W; gx++) {
    float l = level[c][gy][gx] > 1 ? level[c][gy][gx] : 1;
    isp->lsc.gain[c][gy][gx] = top[c] / l;
    if (c == 1 && l / top[c] < darkest) {
     darkest = l / top[c];
    }
   }
  }
 }
 isp->lsc.on = true;
 if (min_ratio != NULL) {
  *min_ratio = darkest;
 }
 return true;
}
static bool
tune_set(const uint8_t **p, const uint8_t *end, struct isp_tune_set *set, size_t floats)
{
 uint32_t n;
 if (end - *p < 4) {
  return false;
 }
 memcpy(&n, *p, 4);
 *p += 4;
 if (n == 0 || n > TUNE_LIGHTS) {
  return false;
 }
 set->lights = n;
 for (uint32_t i = 0; i < n; i++) {
  if (end - *p < 12) {
   return false;
  }
  memcpy(&set->light[i].lux_from, *p, 4);
  memcpy(&set->light[i].lux_to, *p + 4, 4);
  memcpy(&set->light[i].ccts, *p + 8, 4);
  *p += 12;
  if (set->light[i].ccts == 0 || set->light[i].ccts > TUNE_CCTS) {
   return false;
  }
  for (uint32_t j = 0; j < set->light[i].ccts; j++) {
   if ((size_t)(end - *p) < (2 + floats) * 4) {
    return false;
   }
   memcpy(&set->light[i].cct_from[j], *p, 4);
   memcpy(&set->light[i].cct_to[j], *p + 4, 4);
   set->light[i].v[j] = (const float *)(const void *)(*p + 8);
   *p += (2 + floats) * 4;
  }
 }
 return true;
}
bool
isp_load_tuning(struct isp *isp, const char *path)
{
 struct isp_tuning *t = calloc(1, sizeof(*t));
 int fd = open(path, O_RDONLY);
 struct stat st;
 const uint8_t *p, *end;
 bool good = false;
 if (isp->tuning != NULL) {
  free(isp->tuning->file);
  free(isp->tuning);
  isp->tuning = NULL;
 }
 if (t != NULL && fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 8 && st.st_size < (16 << 20) &&
     (t->file = malloc((size_t)st.st_size)) != NULL && read(fd, t->file, (size_t)st.st_size) == st.st_size) {
  p = t->file;
  end = p + st.st_size;
  good = memcmp(p, "RWCISP02", 8) == 0;
  p += 8;
  good = good && tune_set(&p, end, &t->ccm, 9) && tune_set(&p, end, &t->lsc, 4 * LSC_W * LSC_H) && end - p >= 4;
  if (good) {
   memcpy(&t->refs, p, 4);
   p += 4;
   good = t->refs <= 16 && (size_t)(end - p) >= t->refs * 12;
  }
  if (good) {
   memcpy(t->ref, p, t->refs * 12);
  }
 }
 if (fd >= 0) {
  close(fd);
 }
 if (!good) {
  if (t != NULL) {
   free(t->file);
  }
  free(t);
  return false;
 }
 isp->tuning = t;
 return true;
}
static void
pick(const float *from, const float *to, uint32_t n, float x, uint32_t *a, uint32_t *b, float *t)
{
 *a = *b = n - 1;
 *t = 0;
 for (uint32_t i = 0; i < n; i++) {
  if (x <= to[i]) {
   *a = *b = i;
   if (i > 0 && x < from[i]) {
    *a = i - 1;
    *t = (x - to[i - 1]) / (from[i] - to[i - 1]);
   }
   return;
  }
 }
}
static void
tune_blend(const struct isp_tune_set *set, float lux_index, float cct, size_t floats, float *out)
{
 float from[TUNE_LIGHTS], to[TUNE_LIGHTS], tl;
 uint32_t la, lb;
 for (uint32_t i = 0; i < set->lights; i++) {
  from[i] = set->light[i].lux_from;
  to[i] = set->light[i].lux_to;
 }
 pick(from, to, set->lights, lux_index, &la, &lb, &tl);
 memset(out, 0, floats * sizeof(float));
 for (int k = 0; k < 2; k++) {
  uint32_t l = k ? lb : la, ca, cb;
  float w = k ? tl : 1 - tl, tc;
  if (w == 0) {
   continue;
  }
  pick(set->light[l].cct_from, set->light[l].cct_to, set->light[l].ccts, cct, &ca, &cb, &tc);
  for (size_t i = 0; i < floats; i++) {
   out[i] += w * ((1 - tc) * set->light[l].v[ca][i] + tc * set->light[l].v[cb][i]);
  }
 }
}
void
isp_tune(struct isp *isp, float lux_index, float cct)
{
 const struct isp_tuning *t = isp->tuning;
 float m[9], g[4 * LSC_H * LSC_W];
 const int grid = LSC_H * LSC_W;
 if (t == NULL) {
  return;
 }
 tune_blend(&t->ccm, lux_index, cct, 9, m);
 isp_set_ccm(isp, m);
 tune_blend(&t->lsc, lux_index, cct, 4 * (size_t)grid, g);
 for (int gy = 0; gy < LSC_H; gy++) {
  for (int gx = 0; gx < LSC_W; gx++) {
   int at = gy * LSC_W + gx;
   isp->lsc.gain[0][gy][gx] = g[at];
   isp->lsc.gain[1][gy][gx] = (g[grid + at] + g[2 * grid + at]) / 2;
   isp->lsc.gain[2][gy][gx] = g[3 * grid + at];
  }
 }
 isp->lsc.on = true;
}
