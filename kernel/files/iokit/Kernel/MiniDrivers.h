#ifndef _IOKIT_MINIDRIVERS_H
#define _IOKIT_MINIDRIVERS_H
#include <IOKit/IOService.h>
void MiniIOSurfaceStart(IOService * provider);
void MiniIOMobileFramebufferStart(IOService * provider);
void MiniAppleKeyStoreStart(void);
void MiniIONetworkStackStart(void);
#endif
