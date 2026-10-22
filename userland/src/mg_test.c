#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
extern CFTypeRef MGCopyAnswer(CFStringRef key);
extern uint64_t MGGetProductType(void);
static const char *const keys[] = {
 "DeviceClass", "DeviceClassNumber", "DeviceName", "ProductType", "HWModelStr", "ModelNumber", "ProductName",
 "marketing-name", "UserAssignedDeviceName", "ComputerName", "SerialNumber", "UniqueDeviceID", "HasBaseband",
 "telephony", "cellular-data", "wifi", "bluetooth", "DeviceSupportsTelephonyOverUSB", "main-screen-scale",
 "main-screen-width", "main-screen-height", "main-screen-class", "ArtworkDeviceSubType", "DeviceColor", "CPUArchitecture",
 "BuildVersion", "ProductVersion", "RegionInfo", "front-facing-camera", "rear-facing-camera", "still-camera",
};
int
main(int argc, char **argv)
{
 int count = argc > 1 ? argc - 1 : (int)(sizeof(keys) / sizeof(keys[0]));
 for (int i = 0; i < count; i++) {
  const char *name = argc > 1 ? argv[i + 1] : keys[i];
  if (strcmp(name, "MGGetProductType") == 0) {
   printf("mg_test: MGGetProductType() = %#llx\n", (unsigned long long)MGGetProductType());
   continue;
  }
  CFStringRef key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
  CFTypeRef answer = MGCopyAnswer(key);
  CFStringRef text = answer ? CFCopyDescription(answer) : NULL;
  char line[2048] = "(none)";
  if (text) {
   CFStringGetCString(text, line, sizeof(line), kCFStringEncodingUTF8);
   CFRelease(text);
  }
  printf("mg_test: %s = %s\n", name, line);
  syslog(LOG_NOTICE, "mg_test: %s = %s", name, line);
  if (answer) {
   CFRelease(answer);
  }
  CFRelease(key);
 }
 return 0;
}
