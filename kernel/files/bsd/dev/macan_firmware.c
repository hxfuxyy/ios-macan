#include <libkern/libkern.h>
#include <sys/stat.h>
#include <sys/macan_firmware.h>
#include <miscfs/packfs/packfs.h>
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);
static const struct packfs_dnode *
image_entry(const uint8_t *image, const struct packfs_super *super, const struct packfs_dnode *directory, const char *name)
{
 const struct packfs_dnode *nodes = (const struct packfs_dnode *)(image + super->ps_nodes_off);
 size_t length = strlen(name);
 if (directory == NULL || !S_ISDIR(directory->pd_mode) || directory->pd_data + directory->pd_size > super->ps_node_count) {
  return NULL;
 }
 for (uint64_t i = 0; i < directory->pd_size; i++) {
  const struct packfs_dnode *node = &nodes[directory->pd_data + i];
  if (node->pd_name_len == length && (uint64_t)node->pd_name + length <= super->ps_strings_size &&
      memcmp(image + super->ps_strings_off + node->pd_name, name, length) == 0) {
   return node;
  }
 }
 return NULL;
}
const uint8_t *
macan_image_file(const char *directory, const char *name, uint64_t *size)
{
 for (int disk = 1; disk >= 0; disk--) {
  const struct packfs_super *super;
  const struct packfs_dnode *file;
  vm_offset_t base = 0;
  uint64_t length = 0;
  dev_t dev = mdevlookup(disk);
  if (dev == (dev_t)-1 || mdevgetkernelrange(dev, &base, &length) != 0 || length < PACKFS_SUPER_SIZE) {
   continue;
  }
  super = (const struct packfs_super *)base;
  if (memcmp(super->ps_magic, PACKFS_MAGIC, sizeof(super->ps_magic)) != 0 || super->ps_image_size > length ||
      super->ps_node_count == 0 ||
      super->ps_nodes_off + (uint64_t)super->ps_node_count * sizeof(struct packfs_dnode) > super->ps_image_size ||
      super->ps_strings_off + super->ps_strings_size > super->ps_image_size) {
   continue;
  }
  file = (const struct packfs_dnode *)(base + super->ps_nodes_off);
  file = image_entry((const uint8_t *)base, super, file, directory);
  file = image_entry((const uint8_t *)base, super, file, name);
  if (file != NULL && S_ISREG(file->pd_mode) && !(file->pd_flags & PACKFS_F_CHUNKED) &&
      file->pd_data + file->pd_size <= super->ps_image_size) {
   *size = file->pd_size;
   return (const uint8_t *)base + file->pd_data;
  }
 }
 return NULL;
}
