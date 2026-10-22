#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "macan_gl.h"
#define OSMESA_RGBA 0x1908
#define OSMESA_FORMAT 0x22
#define OSMESA_DEPTH_BITS 0x30
#define OSMESA_STENCIL_BITS 0x31
#define OSMESA_PROFILE 0x33
#define OSMESA_CONTEXT_MAJOR_VERSION 0x36
#define OSMESA_CONTEXT_MINOR_VERSION 0x37
#define OSMESA_ES_PROFILE 0x38
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_2D 0x0de1
#define GL_RENDERER 0x1f01
typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);
extern void OSMesaDestroyContext(OSMesaContext context);
extern void (*OSMesaGetProcAddress(const char *name))(void);
extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
#define WINDOW 16
struct redwood_gl {
 OSMesaContext context;
 unsigned char window[WINDOW * WINDOW * 4];
};
struct redwood_gl *
redwood_gl_create(int version)
{
 const int attributes[] = {
  OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
  OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, version, OSMESA_CONTEXT_MINOR_VERSION, 0,
  0
 };
 struct redwood_gl *gl = calloc(1, sizeof(*gl));
 if (gl != NULL && (gl->context = OSMesaCreateContextAttribs(attributes, NULL)) == NULL) {
  free(gl);
  gl = NULL;
 }
 return gl;
}
int
redwood_gl_make_current(struct redwood_gl *gl)
{
 if (gl == NULL) {
  return OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
 }
 return OSMesaMakeCurrent(gl->context, gl->window, GL_UNSIGNED_BYTE, WINDOW, WINDOW);
}
void
redwood_gl_destroy(struct redwood_gl *gl)
{
 OSMesaDestroyContext(gl->context);
 free(gl);
}
const char *
redwood_gl_library(void)
{
 return "Mesa, off-screen (OSMesa)";
}
int
redwood_gl_surface_texture(struct redwood_gl *gl, uint32_t surface)
{
 static void (*image_target)(unsigned target, void *image);
 static int asked, software;
 (void)gl;
 if (!asked) {
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  asked = 1;
  image_target = (void (*)(unsigned, void *))OSMesaGetProcAddress("glEGLImageTargetTexture2DOES");
  software = renderer == NULL || strstr(renderer, "softpipe") != NULL || strstr(renderer, "llvmpipe") != NULL;
 }
 if (image_target == NULL || software) {
  return 0;
 }
 while (glGetError() != 0) {
 }
 image_target(GL_TEXTURE_2D, (void *)(uintptr_t)surface);
 return glGetError() == 0;
}
