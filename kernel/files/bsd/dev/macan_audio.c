#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/macan_audio.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <vm/vm_kern.h>
extern uint64_t kvtophys(vm_offset_t va);
#define CHUNK_FRAMES 1024
#define QUIET_MS 500
#define SILENCE 8
#define REC_MAX_SECONDS 1200
struct rec_header {
 char magic[8];
 uint32_t rate, channels;
 uint32_t blocks;
 uint32_t reserved;
};
struct rec_block {
 uint64_t nanoseconds;
 uint32_t frames;
 uint32_t reserved;
};
extern uint64_t loader_sound[3];
static LCK_GRP_DECLARE(audio_lck_grp, "redwood-audio");
static LCK_MTX_DECLARE(audio_lock, &audio_lck_grp);
static redwood_audio_sink_t audio_sink;
static uint8_t *rec_base;
static bool rec_fed;
static bool rec_mic;
static uint64_t rec_size, rec_used;
static uint64_t rec_frames, rec_frames_max;
static uint64_t stretch_first, stretch_last;
static uint64_t stretch_frames, stretch_latest;
static uint64_t stretch_sound, stretch_lost;
static uint32_t stretch_peak;
static thread_call_t report_call;
static bool report_armed;
static void
audio_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
 uint64_t frames, latest, sound, lost, deadline, since = 0, lasted = 0;
 uint32_t loudest;
 lck_mtx_lock(&audio_lock);
 absolutetime_to_nanoseconds(mach_absolute_time() - stretch_last, &since);
 if (since < QUIET_MS * NSEC_PER_MSEC) {
  clock_interval_to_deadline(2 * QUIET_MS, NSEC_PER_MSEC, &deadline);
  thread_call_enter_delayed(report_call, deadline);
  lck_mtx_unlock(&audio_lock);
  return;
 }
 absolutetime_to_nanoseconds(stretch_last - stretch_first, &lasted);
 frames = stretch_frames;
 latest = stretch_latest;
 sound = stretch_sound;
 lost = stretch_lost;
 loudest = stretch_peak;
 stretch_frames = stretch_sound = stretch_lost = 0;
 stretch_peak = 0;
 report_armed = false;
 lck_mtx_unlock(&audio_lock);
 printf("redwood-audio: %llu.%02llu s written", frames / REDWOOD_AUDIO_RATE,
     frames % REDWOOD_AUDIO_RATE * 100 / REDWOOD_AUDIO_RATE);
 if (lasted >= NSEC_PER_SEC) {
  printf(" at %llu frames a second", (frames - latest) * NSEC_PER_SEC / lasted);
 }
 printf(", %llu.%02llu s of it sound, peak %u%% of full scale", sound / REDWOOD_AUDIO_RATE,
     sound % REDWOOD_AUDIO_RATE * 100 / REDWOOD_AUDIO_RATE, loudest * 100 / 32768);
 if (lost) {
  printf(", %llu ms lost (the sink was full)", lost * 1000 / REDWOOD_AUDIO_RATE);
 }
 printf("\n");
}
static void
audio_record(const int16_t *frames, uint32_t count)
{
 struct rec_header *header = (struct rec_header *)rec_base;
 struct rec_block *block;
 uint64_t bytes = (uint64_t)count * REDWOOD_AUDIO_FRAME;
 if (rec_base == NULL || rec_frames >= rec_frames_max || rec_used + sizeof(*block) + bytes > rec_size) {
  return;
 }
 block = (struct rec_block *)(rec_base + rec_used);
 absolutetime_to_nanoseconds(mach_absolute_time(), &block->nanoseconds);
 block->frames = count;
 block->reserved = 0;
 memcpy(block + 1, frames, bytes);
 rec_used += sizeof(*block) + bytes;
 rec_frames += count;
 header->blocks++;
 loader_sound[1] = rec_used;
 if (rec_frames >= rec_frames_max) {
  printf("redwood-audio: recording finished: %llu s in %u writes\n", rec_frames / REDWOOD_AUDIO_RATE, header->blocks);
 }
}
bool
redwood_audio_recording_fed(void)
{
 return rec_fed && rec_base != NULL && rec_frames < rec_frames_max;
}
bool
redwood_audio_recording_mic(void)
{
 return rec_mic && rec_base != NULL && rec_frames < rec_frames_max;
}
void
redwood_audio_record_fed(const int16_t *frames, uint32_t count)
{
 lck_mtx_lock(&audio_lock);
 audio_record(frames, count);
 lck_mtx_unlock(&audio_lock);
}
static void
audio_play(const int16_t *frames, uint32_t count)
{
 uint32_t loudest = 0, taken = 0;
 uint64_t deadline;
 for (uint32_t i = 0; i < count * REDWOOD_AUDIO_CHANNELS; i++) {
  uint32_t level = frames[i] < 0 ? -(int32_t)frames[i] : frames[i];
  if (level > loudest) {
   loudest = level;
  }
 }
 lck_mtx_lock(&audio_lock);
 if (!rec_fed && !rec_mic) {
  audio_record(frames, count);
 }
 if (audio_sink != NULL) {
  taken = audio_sink(frames, count);
  stretch_lost += count - taken;
 }
 stretch_last = mach_absolute_time();
 if (stretch_frames == 0) {
  stretch_first = stretch_last;
 }
 stretch_frames += count;
 stretch_latest = count;
 if (loudest > SILENCE) {
  stretch_sound += count;
 }
 if (loudest > stretch_peak) {
  stretch_peak = loudest;
 }
 if (!report_armed && report_call != NULL) {
  report_armed = true;
  clock_interval_to_deadline(2 * QUIET_MS, NSEC_PER_MSEC, &deadline);
  thread_call_enter_delayed(report_call, deadline);
 }
 lck_mtx_unlock(&audio_lock);
}
void
redwood_audio_set_sink(const char *name, redwood_audio_sink_t sink)
{
 lck_mtx_lock(&audio_lock);
 audio_sink = sink;
 lck_mtx_unlock(&audio_lock);
 printf("redwood-audio: sound goes to %s\n", name);
}
static int
audio_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
 return 0;
}
static int
audio_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
 return 0;
}
static int
audio_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
 int16_t frames[CHUNK_FRAMES * REDWOOD_AUDIO_CHANNELS];
 int error = 0;
 while (uio_resid(uio) >= REDWOOD_AUDIO_FRAME) {
  user_ssize_t bytes = uio_resid(uio);
  if (bytes > (user_ssize_t)sizeof(frames)) {
   bytes = sizeof(frames);
  }
  bytes -= bytes % REDWOOD_AUDIO_FRAME;
  error = uiomove((caddr_t)frames, (int)bytes, uio);
  if (error) {
   break;
  }
  audio_play(frames, (uint32_t)(bytes / REDWOOD_AUDIO_FRAME));
 }
 return error;
}
static uint32_t volume_below, volume_generation;
static bool volume_muted;
static int
audio_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flags, __unused struct proc *p)
{
 const struct redwood_audio_volume *volume = (const struct redwood_audio_volume *)data;
 if (command != REDWOOD_AUDIO_SET_VOLUME) {
  return ENOTTY;
 }
 lck_mtx_lock(&audio_lock);
 volume_below = volume->below;
 volume_muted = volume->muted != 0;
 volume_generation++;
 lck_mtx_unlock(&audio_lock);
 printf("redwood-audio: the volume: %s%u.%03u dB under the loudest\n", volume->muted ? "muted; " : "", volume->below / 8,
     volume->below % 8 * 125);
 return 0;
}
bool
redwood_audio_volume(uint32_t *below, bool *muted, uint32_t *generation)
{
 lck_mtx_lock(&audio_lock);
 *below = volume_below;
 *muted = volume_muted;
 *generation = volume_generation;
 lck_mtx_unlock(&audio_lock);
 return *generation != 0;
}
static const struct cdevsw audio_cdevsw = {
 .d_open = audio_open, .d_close = audio_close, .d_read = eno_rdwrt, .d_write = audio_write,
 .d_ioctl = audio_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
 .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};
static void
audio_recording_init(void)
{
 struct rec_header *header;
 uint32_t seconds = 0, fed = 0;
 vm_offset_t memory = 0;
 uint64_t size;
 if (!PE_parse_boot_argn("rec_sound", &seconds, sizeof(seconds)) || seconds == 0) {
  return;
 }
 if (seconds > REC_MAX_SECONDS) {
  seconds = REC_MAX_SECONDS;
 }
 size = sizeof(*header) + (uint64_t)seconds * (REDWOOD_AUDIO_RATE * REDWOOD_AUDIO_FRAME + 100 * sizeof(struct rec_block));
 size = round_page(size);
 if (kmem_alloc_contig(kernel_map, &memory, size, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
     VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
  printf("redwood-audio: no memory for a recording of %u s (%llu MB)\n", seconds, size >> 20);
  return;
 }
 header = (struct rec_header *)memory;
 memcpy(header->magic, "RWSND001", 8);
 header->rate = REDWOOD_AUDIO_RATE;
 header->channels = REDWOOD_AUDIO_CHANNELS;
 rec_size = size;
 rec_used = sizeof(*header);
 rec_frames_max = (uint64_t)seconds * REDWOOD_AUDIO_RATE;
 rec_base = (uint8_t *)memory;
 loader_sound[0] = kvtophys(memory);
 loader_sound[1] = rec_used;
 loader_sound[2] = (uint64_t)memory;
 PE_parse_boot_argn("rec_sound_fed", &fed, sizeof(fed));
 rec_fed = fed != 0;
 fed = 0;
 PE_parse_boot_argn("rec_sound_mic", &fed, sizeof(fed));
 rec_mic = fed != 0;
 printf("redwood-audio: recording the first %u s of sound%s, into %llu MB at %#llx\n", seconds,
     rec_fed ? " as the speakers' guard lets it through" : rec_mic ? " the microphone hears (in both channels)" : "", size >> 20,
     loader_sound[0]);
}
void
redwood_audio_init(void)
{
 int major = cdevsw_add(-1, &audio_cdevsw);
 if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-audio") == NULL) {
  printf("redwood-audio: cannot create the device\n");
  return;
 }
 report_call = thread_call_allocate(audio_report, NULL);
 audio_recording_init();
 macan_haptic_init();
 macan_torch_init();
}
