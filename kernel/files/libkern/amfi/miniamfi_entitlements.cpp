#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSUnserialize.h>
#include <kern/cs_blobs.h>
#include <mach/kern_return.h>
#include <kern/kalloc.h>
#include <string.h>
extern "C" {
struct proc;
int cs_entitlements_blob_get(struct proc *p, void **out_start, size_t *out_length);
kern_return_t miniamfi_entitlement_boolean(struct proc *proc, const char *name);
kern_return_t miniamfi_entitlement_string(struct proc *proc, const char *name, const char *value);
kern_return_t miniamfi_entitlement_object(struct proc *proc, const char *name, void **object);
}
static OSDictionary *
copy_entitlements(struct proc *proc)
{
 void *start = NULL;
 size_t length = 0, text_length;
 OSObject *parsed;
 OSDictionary *dict;
 char *text;
 if (proc == NULL || cs_entitlements_blob_get(proc, &start, &length) != 0 || start == NULL ||
     length <= sizeof(CS_GenericBlob)) {
  return NULL;
 }
 text_length = length - sizeof(CS_GenericBlob);
 text = (char *)kalloc_data(text_length + 1, Z_WAITOK);
 if (text == NULL) {
  return NULL;
 }
 memcpy(text, ((const CS_GenericBlob *)start)->data, text_length);
 text[text_length] = '\0';
 parsed = OSUnserializeXML(text, NULL);
 kfree_data(text, text_length + 1);
 dict = OSDynamicCast(OSDictionary, parsed);
 if (dict == NULL && parsed != NULL) {
  parsed->release();
 }
 return dict;
}
kern_return_t
miniamfi_entitlement_boolean(struct proc *proc, const char *name)
{
 OSDictionary *entitlements = copy_entitlements(proc);
 kern_return_t result = KERN_NOT_FOUND;
 if (entitlements != NULL) {
  if (entitlements->getObject(name) == kOSBooleanTrue) {
   result = KERN_SUCCESS;
  }
  entitlements->release();
 }
 return result;
}
kern_return_t
miniamfi_entitlement_string(struct proc *proc, const char *name, const char *value)
{
 OSDictionary *entitlements = copy_entitlements(proc);
 kern_return_t result = KERN_NOT_FOUND;
 if (entitlements != NULL) {
  OSObject *object = entitlements->getObject(name);
  OSString *string = OSDynamicCast(OSString, object);
  OSArray *array = OSDynamicCast(OSArray, object);
  if (string != NULL && string->isEqualTo(value)) {
   result = KERN_SUCCESS;
  }
  for (unsigned int i = 0; array != NULL && i < array->getCount(); i++) {
   string = OSDynamicCast(OSString, array->getObject(i));
   if (string != NULL && string->isEqualTo(value)) {
    result = KERN_SUCCESS;
   }
  }
  entitlements->release();
 }
 return result;
}
kern_return_t
miniamfi_entitlement_object(struct proc *proc, const char *name, void **object)
{
 OSDictionary *entitlements = copy_entitlements(proc);
 OSObject *value = NULL;
 if (entitlements != NULL) {
  value = entitlements->getObject(name);
  if (value != NULL) {
   value->retain();
  }
  entitlements->release();
 }
 *object = value;
 return value != NULL ? KERN_SUCCESS : KERN_NOT_FOUND;
}
