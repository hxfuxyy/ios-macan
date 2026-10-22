#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
extern "C" void RedwoodWLANStart(const char * interface, const uint8_t * address);
class IO80211Controller : public IOService
{
 OSDeclareDefaultStructors(IO80211Controller);
};
OSDefineMetaClassAndStructors(IO80211Controller, IOService);
class IONetworkInterface : public IOService
{
 OSDeclareDefaultStructors(IONetworkInterface);
};
OSDefineMetaClassAndStructors(IONetworkInterface, IOService);
class RedwoodWLAN : public IONetworkInterface
{
 OSDeclareDefaultStructors(RedwoodWLAN);
public:
 virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
     IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(RedwoodWLAN, IONetworkInterface);
IOReturn
RedwoodWLAN::newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties, IOUserClient ** handler)
{
 static unsigned said;
 if (said++ < 8) {
  IOLog("RedwoodWLAN: a user client of type %u is asked for: there is none\n", (unsigned) type);
 }
 return kIOReturnUnsupported;
}
static IOService *
publish(IOService * service, IOService * provider, const char * name)
{
 if (service == NULL || !service->attach(provider)) {
  OSSafeReleaseNULL(service);
  return NULL;
 }
 service->setName(name);
 if (!service->start(provider)) {
  service->detach(provider);
  service->release();
  return NULL;
 }
 service->registerService();
 return service;
}
void
RedwoodWLANStart(const char * interface, const uint8_t * address)
{
 IOService * platform = IOService::getPlatform();
 IO80211Controller * controller = OSTypeAlloc(IO80211Controller);
 RedwoodWLAN * wlan = OSTypeAlloc(RedwoodWLAN);
 OSData * mac;
 OSNumber * number;
 if (platform == NULL || controller == NULL || wlan == NULL || !controller->init() || !wlan->init()) {
  OSSafeReleaseNULL(controller);
  OSSafeReleaseNULL(wlan);
  IOLog("RedwoodWLAN: cannot create the services\n");
  return;
 }
 if ((mac = OSData::withBytes(address, 6)) != NULL) {
  controller->setProperty("IOMACAddress", mac);
  mac->release();
 }
 controller->setProperty("IOBuiltin", true);
 if ((controller = (IO80211Controller *) publish(controller, platform, "RedwoodWLANController")) == NULL) {
  wlan->release();
  IOLog("RedwoodWLAN: the controller did not start\n");
  return;
 }
 wlan->setProperty("IOInterfaceName", interface);
 wlan->setProperty("BSD Name", interface);
 wlan->setProperty("IOInterfaceNamePrefix", "en");
 if ((number = OSNumber::withNumber(6ULL, 32)) != NULL) {
  wlan->setProperty("IOInterfaceType", number);
  number->release();
 }
 if ((number = OSNumber::withNumber((unsigned long long) (interface[2] - '0'), 32)) != NULL) {
  wlan->setProperty("IOInterfaceUnit", number);
  number->release();
 }
 wlan->setProperty("IOBuiltin", true);
 wlan->setProperty("IOPrimaryInterface", true);
 wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
 if (publish(wlan, controller, "RedwoodWLAN") == NULL) {
  IOLog("RedwoodWLAN: the service did not start\n");
 } else {
  wlan->release();
 }
 controller->release();
}
