#ifndef _IOKIT_MINIIOSURFACE_H
#define _IOKIT_MINIIOSURFACE_H
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "MiniDrivers.h"
#define MINISURF_MAX_PLANES 4
#define MINISURF_RESULT_MAX 0xa68
struct minisurf_plane {
 uint32_t width;
 uint32_t height;
 uint32_t reserved08;
 uint32_t offset;
 uint32_t bytesPerRow;
 uint32_t size;
 uint16_t bytesPerElement;
 uint8_t elementWidth;
 uint8_t elementHeight;
 uint8_t componentCount;
 uint8_t addressFormat;
 uint8_t reserved1e[2];
 uint8_t componentBitDepth[4];
 uint8_t componentBitOffset[4];
 uint8_t componentName[4];
 uint8_t componentType[4];
 uint8_t componentRange[4];
 uint8_t reserved34[0x20];
 uint8_t compression;
 uint8_t reserved55[0x1b];
 uint32_t extendedPixels[4];
};
struct minisurf_result {
 uint64_t base;
 uint64_t shared;
 uint64_t aux;
 uint32_t sid;
 uint32_t allocSize;
 uint32_t width;
 uint32_t height;
 uint32_t bytesPerRow;
 uint32_t offset;
 uint32_t pixelFormat;
 uint32_t reserved34;
 uint32_t planeCount;
 uint32_t reserved3c;
 uint16_t bytesPerElement;
 uint8_t elementWidth;
 uint8_t elementHeight;
 uint32_t cacheMode;
 uint32_t lockSeed;
 uint8_t subsampling;
 uint8_t prohibitUseCount;
 uint16_t reserved4e;
 uint32_t parentID;
 uint32_t sliceCount;
 uint64_t traceID;
 uint64_t protectionOptions;
};
struct minisurf_shared {
 uint64_t registryID;
 uint16_t accelID;
 uint16_t accelSubID;
 uint32_t seed;
 uint32_t reserved10;
 uint8_t tiled;
 uint8_t reserved15[3];
 uint64_t useCount;
 uint32_t valuesSeed;
 uint32_t ycbcrMatrix;
 uint32_t valuesSize;
 uint8_t pixelSizeCasting;
 uint8_t displayable;
 uint8_t sysMemOnly;
 uint8_t reserved2f;
 uint8_t attachments[0x70];
};
struct minisurf_aux {
 uint64_t timestamps[4];
 uint64_t detachModeCode;
};
#define MINISURF_AUX_OFFSET 0x400
class MiniIOSurface : public OSObject
{
 OSDeclareDefaultStructors(MiniIOSurface);
public:
 uint32_t sid;
 uint32_t holders;
 IOBufferMemoryDescriptor * memory;
 uint8_t * kbase;
 IOMemoryDescriptor * client;
 IOMemoryMap * clientMap;
 task_t clientTask;
 mach_vm_address_t clientAddress;
 uint8_t * kpixels;
 uint32_t drawn;
 struct minisurf_result info;
 struct minisurf_plane planes[MINISURF_MAX_PLANES];
 OSDictionary * values;
 uint32_t useCount[2];
 uint32_t purgeable;
 bool traced;
 virtual bool
 init(void) APPLE_KEXT_OVERRIDE
 {
  return OSObject::init();
 }
 virtual void free(void) APPLE_KEXT_OVERRIDE;
 struct minisurf_shared *
 shared(void) const
 {
  return (struct minisurf_shared *) kbase;
 }
 struct minisurf_aux *
 aux(void) const
 {
  return (struct minisurf_aux *) (kbase + MINISURF_AUX_OFFSET);
 }
 uint8_t *
 pixels(void) const
 {
  return kpixels + info.offset;
 }
 void updateValuesState(void);
};
MiniIOSurface * MiniIOSurfaceCopy(uint32_t sid);
void MiniIOSurfaceCost(uint32_t * made, uint64_t * making, uint32_t * freed, uint64_t * freeing, uint32_t * again);
void MiniIOSurfaceClearKept(void);
bool MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds);
MiniIOSurface * MiniIOSurfaceCreateSimple(uint32_t width, uint32_t height, uint32_t pixelFormat);
void MiniIOSurfaceDrop(MiniIOSurface * surface);
mach_port_name_t MiniIOSurfaceMakeSendRight(task_t task, MiniIOSurface * surface);
#endif
