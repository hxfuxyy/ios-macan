#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include "MiniDrivers.h"
class IONetworkStack : public IOService
{
 OSDeclareDefaultStructors(IONetworkStack);
public:
 virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IONetworkStack, IOService);
bool
IONetworkStack::start(IOService * provider)
{
 if (!IOService::start(provider)) {
  return false;
 }
 setProperty(kIOUserClientClassKey, "IONetworkStackUserClient");
 setName("IONetworkStack");
 registerService();
 return true;
}
class IONetworkStackUserClient : public IOUserClient
{
 OSDeclareDefaultStructors(IONetworkStackUserClient);
public:
 virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
 virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
 virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);
bool
IONetworkStackUserClient::start(IOService * provider)
{
 char who[32] = "?";
 if (!IOUserClient::start(provider)) {
  return false;
 }
 proc_selfname(who, sizeof(who));
 IOLog("MiniIONetworkStack: opened by %s; the registry's busy state is %u\n", who,
     (unsigned) IOService::getServiceRoot()->getBusyState());
 return true;
}
IOReturn
IONetworkStackUserClient::clientClose(void)
{
 if (!isInactive()) {
  terminate();
 }
 return kIOReturnSuccess;
}
IOReturn
IONetworkStackUserClient::setProperties(OSObject * properties)
{
 static unsigned said;
 if (said++ < 4) {
  IOLog("MiniIONetworkStack: asked to name an interface: there are none of that kind\n");
 }
 return kIOReturnUnsupported;
}
void
MiniIONetworkStackStart(void)
{
 IOService * resources = IOService::getResourceService();
 IONetworkStack * stack = OSTypeAlloc(IONetworkStack);
 if (resources == NULL || stack == NULL || !stack->init() || !stack->attach(resources)) {
  OSSafeReleaseNULL(stack);
  IOLog("MiniIONetworkStack: cannot create the service\n");
  return;
 }
 if (!stack->start(resources)) {
  stack->detach(resources);
 }
 stack->release();
}
