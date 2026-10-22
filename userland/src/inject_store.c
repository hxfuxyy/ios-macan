#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
extern const void *_dyld_get_shared_cache_range(size_t *length);
#define CACHE_BASE 0x180000000UL
#define STOREFRONT "143469-2,8"
static uintptr_t slide;
static CFStringRef storefront;
struct replacement {
 const char *class_name;
 const char *selector;
 bool class_method;
 IMP replacement;
 IMP *original;
 bool done;
};
static struct replacement replacements[];
static size_t replacement_count(void);
static void image_added(const struct mach_header *header, intptr_t image_slide);
enum {
 COUNT_ams_storefront, COUNT_cachedStorefront, COUNT_bagStorefront_fixed, COUNT_bagStorefront_active,
 COUNT_copyStorefront, COUNT_bagAccount, COUNT_performFetch, COUNT_queryItems, COUNT_performRequest,
 COUNT_performRequestBlock, COUNT_resume, COUNT_resumeLocal, COUNT_createRequest, COUNT_sessionPromise,
 COUNT_dataTaskPromise, COUNT_dataTask, COUNT_addCookies, COUNT_addStorefrontHeader, COUNT_addAnisette,
 COUNT_prepareRequest, COUNT_finishWithResult, COUNT_finishWithError, COUNT_finishFunnel,
 COUNT_finishResultError, COUNT_thenWithBlock, COUNT_continueWithBlock, COUNT_bagValue,
 COUNT_resultBefore, COUNT_afterUpdating, COUNT_addCookies4, COUNT_fpDeviceType, COUNT_brotli,
 COUNT_processStream, COUNT_updateStorefront, COUNT_addCookiesBlocking, COUNT_dataTaskCreated,
 COUNT_last
};
static const char *const counted[COUNT_last] = {
 "-[ACAccount ams_storefront]", "-[AMSBagNetworkDataSource cachedStorefront]",
 "-[AMSBagFixedAccountProvider bagStorefrontForAccountMediaType:]",
 "-[AMSBagActiveAccountProvider bagStorefrontForAccountMediaType:]",
 "-[ACAccount ams_copyStorefrontFromAccount:]",
 "-[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]",
 "-[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:]",
 "-[AMSBagNetworkTask _queryItemsForClientInfo:...]", "-[AMSURLSession performRequest:completion:]",
 "-[AMSURLSession performRequest:completionBlock:]", "-[NSURLSessionTask resume]",
 "-[__NSCFLocalSessionTask resume]", "-[AMSBagNetworkTask _createRequestWithQueryItems:]",
 "-[AMSBagNetworkTask URLSessionPromise]", "-[AMSURLSession dataTaskPromiseWithRequest:]",
 "-[AMSURLSession dataTaskWithRequest:]", "ams_addCookiesForAccount:clientInfo:bag:",
 "ams_addStoreFrontHeaderForAccount:client:withBag:", "ams_addAnisetteHeadersForAccount:type:bag:",
 "-[AMSURLSession _prepareRequest:properties:error:]", "-[AMSMutablePromise finishWithResult:]",
 "-[AMSMutablePromise finishWithError:]", "-[AMSMutablePromise _finishWithResult:error:logDuplicateFinishes:]",
 "-[AMSMutablePromise finishWithResult:error:]", "-[AMSMutablePromise thenWithBlock:]",
 "-[AMSMutablePromise continueWithBlock:]", "-[AMSBagValue valueWithError:]",
 "-[AMSPromise resultBeforeDate:error:]",
 "-[AMSBagValue valueWithError:]", "-[AMSPromise resultBeforeDate:error:]",
 "-[ACAccountStore _ams_localiTunesAccountAfterUpdatingStorefront:]",
 "ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies:",
 "-[AMSUserAgent _sharedComponentFairPlayDeviceType]",
 "-[JEBrotliDecoder decompressStreamWithAvailableIn:nextIn:availableOut:nextOut:]",
 "-[JPReader processStream:signatureVerifier:options:error:]",
 "-[ACAccountStore _updateStorefrontInLocalAccount:]",
 "ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: (answered empty)",
 "-[NSURLSession dataTaskWithRequest:...] (created, not resumed)",
};
static unsigned long entered[COUNT_last];
static unsigned long returned[COUNT_last];
static unsigned serial;
static const char *who = "store";
static void
say(const char *format, ...)
{
 static int console = -2;
 char line[512];
 va_list ap;
 int n;
 va_start(ap, format);
 n = vsnprintf(line, sizeof(line), format, ap);
 va_end(ap);
 if (n > (int)sizeof(line) - 1) {
  n = sizeof(line) - 1;
 }
 if (console == -2) {
  console = open("/dev/console", O_WRONLY | O_APPEND);
 }
 if (strncmp(line, "redwood: store: ", 16) == 0) {
  char named[sizeof(line) + 32];
  n = snprintf(named, sizeof(named), "redwood: %s: %s", who, line + 16);
  if (n > (int)sizeof(named) - 1) {
   n = sizeof(named) - 1;
  }
  write(console >= 0 ? console : 2, named, (size_t)n);
  return;
 }
 write(console >= 0 ? console : 2, line, (size_t)n);
}
static const char *
text_of(id object, char *buffer, size_t size)
{
 CFStringRef description;
 buffer[0] = 0;
 if (object == NULL) {
  return "(nil)";
 }
 description = (CFStringRef)((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description"));
 if (description == NULL || !CFStringGetCString(description, buffer, (CFIndex)size, kCFStringEncodingUTF8)) {
  return "(?)";
 }
 return buffer;
}
static unsigned
step_in(int which)
{
 unsigned n = __sync_add_and_fetch(&serial, 1);
 entered[which]++;
 say("redwood: store: -> #%u %s\n", n, counted[which]);
 return n;
}
static void
step_out(int which, unsigned n, bool got_something)
{
 returned[which]++;
 say("redwood: store: <- #%u %s%s\n", n, counted[which], got_something ? "" : " (nil)");
}
static void
frames_from(uintptr_t fp, char *text, size_t size)
{
 size_t used = 0;
 text[0] = 0;
 for (unsigned i = 0; i < 10 && fp != 0 && (fp & 7) == 0 && used + 20 < size; i++) {
  uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
  if (ret == 0) {
   break;
  }
  used += (size_t)snprintf(text + used, size - used, " %#lx", ret - slide);
  fp = next > fp ? next : 0;
 }
}
#define TASKS 64
static id tasks[TASKS];
static unsigned task_count;
static pthread_mutex_t task_lock = PTHREAD_MUTEX_INITIALIZER;
static void
remember_task(id task)
{
 pthread_mutex_lock(&task_lock);
 if (task_count < TASKS) {
  tasks[task_count++] = (id)CFRetain((CFTypeRef)task);
 }
 pthread_mutex_unlock(&task_lock);
}
static void
dump_tasks(void)
{
 static const char *const states[] = { "running", "suspended", "cancelling", "COMPLETED" };
 say("redwood: store: %u tasks resumed\n", task_count);
 for (unsigned i = 0; i < task_count; i++) {
  id task = tasks[i];
  long state = ((long (*)(id, SEL))objc_msgSend)(task, sel_registerName("state"));
  id response = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("response"));
  id error = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("error"));
  id request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("originalRequest"));
  id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL")) : NULL;
  long long got = ((long long (*)(id, SEL))objc_msgSend)(task, sel_registerName("countOfBytesReceived"));
  long status = response != NULL ? ((long (*)(id, SEL))objc_msgSend)(response, sel_registerName("statusCode")) : 0;
  id type = response != NULL ? ((id (*)(id, SEL))objc_msgSend)(response, sel_registerName("MIMEType")) : NULL;
  char one[96], two[64], three[200];
  say("redwood: store: task %u %s status %ld, %lld bytes, %s%s%s, %s\n", i,
      state >= 0 && state < 4 ? states[state] : "?", status, got,
      type != NULL ? text_of(type, one, sizeof(one)) : "(no type)",
      error != NULL ? ", error " : "", error != NULL ? text_of(error, two, sizeof(two)) : "",
      url != NULL ? text_of(url, three, sizeof(three)) : "(no url)");
 }
}
static long (*old_brotli)(id, SEL, void *, void *, void *, void *);
static long
new_brotli(id self, SEL cmd, void *a, void *b, void *c, void *d)
{
 long result;
 entered[COUNT_brotli]++;
 result = old_brotli(self, cmd, a, b, c, d);
 returned[COUNT_brotli]++;
 if (returned[COUNT_brotli] <= 4 || result != 1) {
  say("redwood: store: BROTLI decompressStream -> %ld (call %lu)\n", result, returned[COUNT_brotli]);
 }
 return result;
}
static id (*old_dataTaskLocal)(id, SEL, void *, void *);
static id
new_dataTaskLocal(id self, SEL cmd, void *request, void *handler)
{
 id task;
 char buffer[260];
 entered[COUNT_dataTaskCreated]++;
 task = ((id (*)(id, SEL, void *, void *))old_dataTaskLocal)(self, cmd, request, handler);
 returned[COUNT_dataTaskCreated]++;
 say("redwood: store: TASK CREATED %s\n", text_of((id)request, buffer, sizeof(buffer)));
 return task;
}
static id (*old_dataTaskAMS)(id, SEL, void *, void *);
static id
new_dataTaskAMS(id self, SEL cmd, void *request, void *handler)
{
 id task;
 char buffer[260];
 entered[COUNT_dataTaskCreated]++;
 task = ((id (*)(id, SEL, void *, void *))old_dataTaskAMS)(self, cmd, request, handler);
 returned[COUNT_dataTaskCreated]++;
 say("redwood: store: TASK CREATED (ams session) %s\n", text_of((id)request, buffer, sizeof(buffer)));
 return task;
}
static id (*old_dataTaskURL)(id, SEL, void *, void *);
static id
new_dataTaskURL(id self, SEL cmd, void *url, void *handler)
{
 id task;
 char buffer[260];
 entered[COUNT_dataTaskCreated]++;
 task = ((id (*)(id, SEL, void *, void *))old_dataTaskURL)(self, cmd, url, handler);
 returned[COUNT_dataTaskCreated]++;
 say("redwood: store: TASK CREATED (url) %s\n", text_of((id)url, buffer, sizeof(buffer)));
 return task;
}
static id (*old_dataTaskSignpost)(id, SEL, void *, void *, void *);
static id
new_dataTaskSignpost(id self, SEL cmd, void *request, void *signpost, void *handler)
{
 id task;
 char buffer[260];
 entered[COUNT_dataTaskCreated]++;
 task = ((id (*)(id, SEL, void *, void *, void *))old_dataTaskSignpost)(self, cmd, request, signpost, handler);
 returned[COUNT_dataTaskCreated]++;
 say("redwood: store: TASK CREATED (ams) %s\n", text_of((id)request, buffer, sizeof(buffer)));
 return task;
}
static id (*old_dataTaskPlain)(id, SEL, void *);
static id
new_dataTaskPlain(id self, SEL cmd, void *request)
{
 id task;
 char buffer[260];
 entered[COUNT_dataTaskCreated]++;
 task = ((id (*)(id, SEL, void *))old_dataTaskPlain)(self, cmd, request);
 returned[COUNT_dataTaskCreated]++;
 say("redwood: store: TASK CREATED (no handler) %s\n", text_of((id)request, buffer, sizeof(buffer)));
 return task;
}
static void
dump_classes(const char *needle)
{
 unsigned count = 0;
 Class *classes = objc_copyClassList(&count);
 unsigned shown = 0;
 if (classes == NULL) {
  return;
 }
 for (unsigned i = 0; i < count && shown < 40; i++) {
  const char *name = class_getName(classes[i]);
  if (name != NULL && strstr(name, needle) != NULL) {
   shown++;
   say("redwood: store: class %s\n", name);
  }
 }
 free(classes);
 say("redwood: store: %u classes in all, %u named like \"%s\"\n", count, shown, needle);
}
static void *(*old_updateStorefront)(id, SEL, void *);
static void *
new_updateStorefront(id self, SEL cmd, void *account)
{
 (void)cmd;
 (void)account;
 entered[COUNT_updateStorefront]++;
 returned[COUNT_updateStorefront]++;
 say("redwood: store: INJECTED _updateStorefrontInLocalAccount: answered at once, no update\n");
 return (void *)((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("ams_localiTunesAccount"));
}
static void *(*old_addCookiesBlocking)(id, SEL, void *, void *, void *, void *);
static void *
new_addCookiesBlocking(id self, SEL cmd, void *account, void *client, void *bag, void *cleanup)
{
 (void)self;
 (void)cmd;
 (void)account;
 (void)client;
 (void)bag;
 (void)cleanup;
 entered[COUNT_addCookiesBlocking]++;
 returned[COUNT_addCookiesBlocking]++;
 say("redwood: store: INJECTED ams_addCookiesForAccount:...:cleanupGlobalCookies: answered with "
     "NO COOKIES (not measured; the real one waits for ever on amsaccountsd)\n");
 return NULL;
}
static id (*old_processStream)(id, SEL, void *, void *, void *, id *);
static id
new_processStream(id self, SEL cmd, void *stream, void *verifier, void *options, id *error)
{
 id result;
 char one[200];
 entered[COUNT_processStream]++;
 say("redwood: store: JETPACK processStream: in\n");
 result = old_processStream(self, cmd, stream, verifier, options, error);
 returned[COUNT_processStream]++;
 {
  char frames[10 * 20 + 1] = "";
  frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
  say("redwood: store: JETPACK processStream: out, %s, error %s, to%s\n",
      result != NULL ? "a result" : "(nil)",
      error != NULL && *error != NULL ? text_of(*error, one, sizeof(one)) : "(none)", frames);
 }
 return result;
}
static unsigned long notifications;
static bool
interesting_name(const char *name)
{
 static const char *const words[] = { "jet", "Jet", "ignature", "erif", "rotli", "JE", "Pack" };
 for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
  if (strstr(name, words[i]) != NULL) {
   return true;
  }
 }
 return false;
}
static id (*old_postName)(id, SEL, id, id, id);
static id
new_postName(id self, SEL cmd, id name, id object, id info)
{
 char buffer[160];
 const char *text = text_of(name, buffer, sizeof(buffer));
 notifications++;
 if (interesting_name(text)) {
  say("redwood: store: NOTIFICATION %s\n", text);
 }
 return old_postName(self, cmd, name, object, info);
}
static id (*old_postNotification)(id, SEL, id);
static id
new_postNotification(id self, SEL cmd, id note)
{
 char buffer[160];
 id name = ((id (*)(id, SEL))objc_msgSend)(note, sel_registerName("name"));
 const char *text = text_of(name, buffer, sizeof(buffer));
 notifications++;
 if (interesting_name(text)) {
  say("redwood: store: NOTIFICATION (object) %s\n", text);
 }
 return old_postNotification(self, cmd, note);
}
#define ERRORS 64
static struct error_note {
 char domain[48];
 long code;
 unsigned count;
} errors[ERRORS];
static unsigned error_count;
static pthread_mutex_t error_lock = PTHREAD_MUTEX_INITIALIZER;
static void
note_error(id domain, long code)
{
 char text[48];
 if (domain == NULL) {
  return;
 }
 text_of(domain, text, sizeof(text));
 pthread_mutex_lock(&error_lock);
 for (unsigned i = 0; i < error_count; i++) {
  if (errors[i].code == code && strncmp(errors[i].domain, text, sizeof(errors[0].domain) - 1) == 0) {
   errors[i].count++;
   pthread_mutex_unlock(&error_lock);
   return;
  }
 }
 if (error_count < ERRORS) {
  strlcpy(errors[error_count].domain, text, sizeof(errors[0].domain));
  errors[error_count].code = code;
  errors[error_count].count = 1;
  error_count++;
 }
 pthread_mutex_unlock(&error_lock);
}
static void
dump_errors(void)
{
 say("redwood: store: %u kinds of error made\n", error_count);
 for (unsigned i = 0; i < error_count; i++) {
  say("redwood: store: error %-40s code %ld, %u time(s)\n", errors[i].domain, errors[i].code,
      errors[i].count);
 }
}
static id (*old_errorWith)(id, SEL, void *, long, void *);
static id
new_errorWith(id self, SEL cmd, void *domain, long code, void *info)
{
 note_error((id)domain, code);
 return old_errorWith(self, cmd, domain, code, info);
}
static id (*old_errorInit)(id, SEL, void *, long, void *);
static id
new_errorInit(id self, SEL cmd, void *domain, long code, void *info)
{
 note_error((id)domain, code);
 return old_errorInit(self, cmd, domain, code, info);
}
#define BAGKEYS 64
static struct bagkey_note {
 char key[72];
 unsigned asked;
 unsigned answered;
} bagkeys[BAGKEYS];
static unsigned bagkey_count;
static pthread_mutex_t bagkey_lock = PTHREAD_MUTEX_INITIALIZER;
static void
note_bagkey(const char *key, bool answered)
{
 pthread_mutex_lock(&bagkey_lock);
 for (unsigned i = 0; i < bagkey_count; i++) {
  if (strncmp(bagkeys[i].key, key, sizeof(bagkeys[0].key) - 1) == 0) {
   bagkeys[i].asked++;
   bagkeys[i].answered += answered ? 1 : 0;
   pthread_mutex_unlock(&bagkey_lock);
   return;
  }
 }
 if (bagkey_count < BAGKEYS) {
  strlcpy(bagkeys[bagkey_count].key, key, sizeof(bagkeys[0].key));
  bagkeys[bagkey_count].asked = 1;
  bagkeys[bagkey_count].answered = answered ? 1 : 0;
  bagkey_count++;
 }
 pthread_mutex_unlock(&bagkey_lock);
}
static void
dump_bagkeys(void)
{
 say("redwood: store: %u bag keys asked for\n", bagkey_count);
 for (unsigned i = 0; i < bagkey_count; i++) {
  say("redwood: store: bag key %-44s asked %u, answered %u%s\n", bagkeys[i].key, bagkeys[i].asked,
      bagkeys[i].answered, bagkeys[i].answered == 0 ? "   <-- THE BAG HAS NOT GOT IT" : "");
 }
}
#define PROMISES 192
struct promise_note {
 void *promise;
 const char *label;
 unsigned waiters;
 unsigned sync_waiters;
 unsigned finished;
 uint64_t tid;
 char frames[160];
};
static struct promise_note promises[PROMISES];
static unsigned promise_count;
static pthread_mutex_t promise_lock = PTHREAD_MUTEX_INITIALIZER;
static struct promise_note *
note_for(void *promise, const char *frames)
{
 for (unsigned i = 0; i < promise_count; i++) {
  if (promises[i].promise == promise) {
   return &promises[i];
  }
 }
 if (promise_count >= PROMISES) {
  return NULL;
 }
 promises[promise_count].promise = promise;
 if (frames != NULL) {
  strlcpy(promises[promise_count].frames, frames, sizeof(promises[0].frames));
 }
 return &promises[promise_count++];
}
static void
note_waiter(void *promise, const char *frames, bool synchronous)
{
 struct promise_note *note;
 uint64_t tid = 0;
 pthread_threadid_np(NULL, &tid);
 pthread_mutex_lock(&promise_lock);
 note = note_for(promise, frames);
 if (note != NULL) {
  note->waiters++;
  if (synchronous) {
   note->sync_waiters++;
   note->tid = tid;
  }
 }
 pthread_mutex_unlock(&promise_lock);
}
static void
note_finished(void *promise)
{
 struct promise_note *note;
 pthread_mutex_lock(&promise_lock);
 note = note_for(promise, NULL);
 if (note != NULL) {
  note->finished++;
 }
 pthread_mutex_unlock(&promise_lock);
}
static void
note_label(void *promise, const char *label, const char *frames)
{
 struct promise_note *note;
 pthread_mutex_lock(&promise_lock);
 note = note_for(promise, frames);
 if (note != NULL) {
  note->label = label;
 }
 pthread_mutex_unlock(&promise_lock);
}
static void
dump_promises(void)
{
 say("redwood: store: %u promises seen\n", promise_count);
 for (unsigned i = 0; i < promise_count; i++) {
  struct promise_note *note = &promises[i];
  say("redwood: store: promise %p %-16s waiters %2u sync %u finished %u%s tid %llu seen from%s\n",
      note->promise, note->label != NULL ? note->label : "", note->waiters, note->sync_waiters,
      note->finished,
      note->finished == 0 && note->sync_waiters > 0 ? "  <-- NOBODY FULFILS IT" : "",
      (unsigned long long)note->tid, note->frames);
 }
}
static void
dump_threads(void)
{
 thread_act_array_t threads = NULL;
 mach_msg_type_number_t count = 0;
 mach_port_t me = mach_task_self();
 if (task_threads(me, &threads, &count) != KERN_SUCCESS) {
  say("redwood: store: task_threads() refused\n");
  return;
 }
 say("redwood: store: %u threads\n", count);
 for (unsigned i = 0; i < count; i++) {
  arm_thread_state64_t state;
  mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
  struct thread_basic_info basic;
  mach_msg_type_number_t basic_count = THREAD_BASIC_INFO_COUNT;
  char text[10 * 20 + 1] = "";
  int run_state = -1, suspend = -1;
  if (thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&basic, &basic_count) == KERN_SUCCESS) {
   run_state = basic.run_state;
   suspend = basic.suspend_count;
  }
  if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &state_count) != KERN_SUCCESS) {
   say("redwood: store: thread %u: no state (run %d)\n", i, run_state);
   continue;
  }
  frames_from((uintptr_t)arm_thread_state64_get_fp(state), text, sizeof(text));
  say("redwood: store: thread %u: run %d suspend %d pc %#lx lr %#lx from%s\n", i, run_state, suspend,
      (uintptr_t)arm_thread_state64_get_pc(state) - slide,
      (uintptr_t)arm_thread_state64_get_lr(state) - slide, text);
 }
 for (unsigned i = 0; i < count; i++) {
  mach_port_deallocate(me, threads[i]);
 }
 vm_deallocate(me, (vm_address_t)threads, count * sizeof(threads[0]));
}
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
 ucontext_t *context = context_in;
 uintptr_t pc = 0, lr = 0, fp = 0;
 char frames[10 * 20 + 1] = "";
 if (context != NULL && context->uc_mcontext != NULL) {
  pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
  lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
  fp = (uintptr_t)context->uc_mcontext->__ss.__fp;
 }
 frames_from(fp, frames, sizeof(frames));
 say("redwood: store: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
     info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
 signal(signal_number, SIG_DFL);
 raise(signal_number);
}
static void
watch_for_faults(void)
{
 struct sigaction action;
 memset(&action, 0, sizeof(action));
 action.sa_sigaction = say_the_fault;
 action.sa_flags = SA_SIGINFO | SA_RESETHAND;
 sigaction(SIGSEGV, &action, NULL);
 sigaction(SIGBUS, &action, NULL);
 sigaction(SIGABRT, &action, NULL);
 sigaction(SIGILL, &action, NULL);
 sigaction(SIGTRAP, &action, NULL);
}
static id (*old_ams_storefront)(id, SEL);
static id (*old_cached_storefront)(id, SEL);
static id (*old_bag_storefront)(id, SEL, id);
static id (*old_bag_storefront_active)(id, SEL, id);
static id (*old_copy_storefront)(id, SEL, id);
static id (*old_bag_account)(id, SEL, id, bool);
static id (*old_perform_fetch)(id, SEL, long, id, id);
static id (*old_query_items)(id, SEL, id, id, id, id, id, id);
static id (*old_perform_request)(id, SEL, id, id);
static id (*old_perform_request_block)(id, SEL, id, id);
static void (*old_resume_base)(id, SEL);
static void (*old_resume_local)(id, SEL);
static id (*old_create_request)(id, SEL, id);
static id (*old_session_promise)(id, SEL);
static id (*old_data_task_promise)(id, SEL, id);
static id (*old_data_task)(id, SEL, id);
static id
new_ams_storefront(id self, SEL _cmd)
{
 entered[COUNT_ams_storefront]++;
 id was = old_ams_storefront != NULL ? old_ams_storefront(self, _cmd) : NULL;
 static bool said;
 if (!said) {
  char buffer[128];
  said = true;
  say("redwood: store: ams_storefront was %s, now " STOREFRONT "\n", text_of(was, buffer, sizeof(buffer)));
 }
 returned[COUNT_ams_storefront]++;
 return was != NULL ? was : (id)storefront;
}
static id
new_cached_storefront(id self, SEL _cmd)
{
 entered[COUNT_cachedStorefront]++;
 id was = old_cached_storefront != NULL ? old_cached_storefront(self, _cmd) : NULL;
 say("redwood: store: cachedStorefront was %s\n", was != NULL ? "set" : "(nil), now " STOREFRONT);
 returned[COUNT_cachedStorefront]++;
 return was != NULL ? was : (id)storefront;
}
static id
new_bag_storefront(id self, SEL _cmd, id media_type)
{
 entered[COUNT_bagStorefront_fixed]++;
 id was = old_bag_storefront != NULL ? old_bag_storefront(self, _cmd, media_type) : NULL;
 char buffer[128];
 say("redwood: store: bagStorefrontForAccountMediaType:%s was %s\n", text_of(media_type, buffer, sizeof(buffer)),
     was != NULL ? "set" : "(nil), now " STOREFRONT);
 returned[COUNT_bagStorefront_fixed]++;
 return was != NULL ? was : (id)storefront;
}
static id
new_bag_storefront_active(id self, SEL _cmd, id media_type)
{
 entered[COUNT_bagStorefront_active]++;
 id was = old_bag_storefront_active != NULL ? old_bag_storefront_active(self, _cmd, media_type) : NULL;
 char buffer[128];
 say("redwood: store: active bagStorefrontForAccountMediaType:%s was %s\n",
     text_of(media_type, buffer, sizeof(buffer)), was != NULL ? "set" : "(nil), now " STOREFRONT);
 returned[COUNT_bagStorefront_active]++;
 return was != NULL ? was : (id)storefront;
}
static id
new_copy_storefront(id self, SEL _cmd, id account)
{
 entered[COUNT_copyStorefront]++;
 id was = old_copy_storefront != NULL ? old_copy_storefront(self, _cmd, account) : NULL;
 say("redwood: store: ams_copyStorefrontFromAccount: was %s\n",
     was != NULL ? "set" : "(nil), now " STOREFRONT);
 returned[COUNT_copyStorefront]++;
 return was != NULL ? was : (id)storefront;
}
static id
new_bag_account(id self, SEL _cmd, id media_type, bool must_contain_storefront)
{
 entered[COUNT_bagAccount]++;
 id account = old_bag_account(self, _cmd, media_type, must_contain_storefront);
 returned[COUNT_bagAccount]++;
 char one[128], two[192];
 say("redwood: store: _bagAccountForAccountMediaType:%s mustContainStorefront:%d -> %s\n",
     text_of(media_type, one, sizeof(one)), must_contain_storefront ? 1 : 0, text_of(account, two, sizeof(two)));
 return account;
}
static id
new_perform_fetch(id self, SEL _cmd, long attempt, id account, id front)
{
 entered[COUNT_performFetch]++;
 char one[160], two[64];
 id promise;
 say("redwood: store: BAG FETCH attempt %ld, storefront %s, account %s\n", attempt,
     text_of(front, two, sizeof(two)), text_of(account, one, sizeof(one)));
 for (size_t i = 0; i < replacement_count(); i++) {
  if (!replacements[i].done) {
   say("redwood: store: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
       replacements[i].class_name, replacements[i].selector);
  }
 }
 promise = old_perform_fetch(self, _cmd, attempt, account, front);
 returned[COUNT_performFetch]++;
 note_label(promise, "THE BAG FETCH", NULL);
 say("redwood: store: BAG FETCH returned %s\n", promise != NULL ? "a promise" : "(nil)");
 return promise;
}
static id
new_query_items(id self, SEL _cmd, id client, id profile, id version, id cookies, id account, id front)
{
 entered[COUNT_queryItems]++;
 id items = old_query_items(self, _cmd, client, profile, version, cookies, account, front);
 char one[64], two[160];
 say("redwood: store: BAG QUERY profile %s, storefront %s -> %s\n", text_of(profile, one, sizeof(one)),
     text_of(front, two, sizeof(two)), items != NULL ? "items" : "(nil)");
 return items;
}
static id
new_perform_request(id self, SEL _cmd, id request, id completion)
{
 entered[COUNT_performRequest]++;
 char buffer[320];
 say("redwood: store: AMSURLSession performRequest:completion: %s\n", text_of(request, buffer, sizeof(buffer)));
 return old_perform_request(self, _cmd, request, completion);
}
static id
new_perform_request_block(id self, SEL _cmd, id request, id completion)
{
 entered[COUNT_performRequestBlock]++;
 char buffer[320];
 say("redwood: store: AMSURLSession performRequest:completionBlock: %s\n", text_of(request, buffer, sizeof(buffer)));
 return old_perform_request_block(self, _cmd, request, completion);
}
static void
say_resume(id task, const char *where)
{
 char buffer[320];
 id request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("originalRequest"));
 say("redwood: store: TASK RESUME (%s) %s\n", where, text_of(request, buffer, sizeof(buffer)));
}
static void
new_resume_base(id self, SEL _cmd)
{
 entered[COUNT_resume]++;
 say_resume(self, "NSURLSessionTask");
 remember_task(self);
 old_resume_base(self, _cmd);
 returned[COUNT_resume]++;
}
static void
new_resume_local(id self, SEL _cmd)
{
 entered[COUNT_resumeLocal]++;
 say_resume(self, "__NSCFLocalSessionTask");
 old_resume_local(self, _cmd);
}
static void
dump_selectors(const char *class_name, const char *needle)
{
 Class cls = objc_getClass(class_name);
 unsigned count = 0;
 Method *methods = cls != NULL ? class_copyMethodList(cls, &count) : NULL;
 if (methods == NULL) {
  say("redwood: store: %s: no such class\n", class_name);
  return;
 }
 for (unsigned i = 0; i < count; i++) {
  const char *name = sel_getName(method_getName(methods[i]));
  if (needle == NULL || strstr(name, needle) != NULL) {
   say("redwood: store: %s has -%s\n", class_name, name);
  }
 }
 free(methods);
}
static void
one_report(unsigned seconds)
{
 bool first = seconds <= 15;
 say("redwood: store(%s): report at %u s\n", who, seconds);
 image_added(NULL, 0);
 say("redwood: store: KEY brotli entered %lu returned %lu, processStream entered %lu returned %lu\n",
     entered[COUNT_brotli], returned[COUNT_brotli], entered[COUNT_processStream], returned[COUNT_processStream]);
 for (size_t i = 0; i < replacement_count(); i++) {
  if (!replacements[i].done) {
   say("redwood: store: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
       replacements[i].class_name, replacements[i].selector);
  }
 }
 for (int i = 0; i < COUNT_last; i++) {
  say("redwood: store: entered %4lu returned %4lu %s%s\n", entered[i], returned[i], counted[i],
      entered[i] > returned[i] ? "   <-- STILL IN IT" :
      entered[i] == 0 ? "   <-- NEVER ENTERED: suspect the hook, not the code" : "");
 }
 dump_promises();
 dump_tasks();
 dump_bagkeys();
 dump_errors();
 dump_threads();
 if (first) {
  dump_selectors("AMSURLSession", "equest");
  dump_selectors("JEBrotliDecoder", NULL);
  dump_selectors("_TtGC9JetEngine7PromiseT__", NULL);
  dump_selectors("JetEngine", NULL);
  dump_classes("Jet");
  dump_classes("JE");
 }
 say("redwood: store: %lu notifications posted\n", notifications);
}
static void *
late_report(__unused void *ignored)
{
 for (unsigned n = 1; n <= 40; n++) {
  sleep(15);
  one_report(n * 15);
 }
 return NULL;
}
static id
new_create_request(id self, SEL _cmd, id items)
{
 id request;
 char buffer[320];
 entered[COUNT_createRequest]++;
 request = old_create_request(self, _cmd, items);
 returned[COUNT_createRequest]++;
 say("redwood: store: STEP _createRequestWithQueryItems: -> %s\n", text_of(request, buffer, sizeof(buffer)));
 note_label(request, "THE BAG REQUEST", NULL);
 return request;
}
static id
new_session_promise(id self, SEL _cmd)
{
 id promise;
 char buffer[200];
 entered[COUNT_sessionPromise]++;
 promise = old_session_promise(self, _cmd);
 returned[COUNT_sessionPromise]++;
 say("redwood: store: STEP URLSessionPromise -> %s\n", text_of(promise, buffer, sizeof(buffer)));
 return promise;
}
static id
new_data_task_promise(id self, SEL _cmd, id request)
{
 id promise;
 char buffer[200];
 entered[COUNT_dataTaskPromise]++;
 promise = old_data_task_promise(self, _cmd, request);
 returned[COUNT_dataTaskPromise]++;
 say("redwood: store: STEP dataTaskPromiseWithRequest: -> %s\n", text_of(promise, buffer, sizeof(buffer)));
 return promise;
}
static id
new_data_task(id self, SEL _cmd, id request)
{
 id task;
 char buffer[200];
 entered[COUNT_dataTask]++;
 task = old_data_task(self, _cmd, request);
 returned[COUNT_dataTask]++;
 say("redwood: store: STEP dataTaskWithRequest: -> %s\n", text_of(task, buffer, sizeof(buffer)));
 return task;
}
#define WRAP4(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c, void *d) \
{ \
 unsigned n = step_in(COUNT_##name); \
 void *result = old_##name(self, cmd, a, b, c, d); \
 step_out(COUNT_##name, n, result != NULL); \
 return result; \
}
#define WRAP3(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c) \
{ \
 unsigned n = step_in(COUNT_##name); \
 void *result = old_##name(self, cmd, a, b, c); \
 step_out(COUNT_##name, n, result != NULL); \
 return result; \
}
#define WRAP1(name) \
static void *(*old_##name)(id, SEL, void *); \
static void * \
new_##name(id self, SEL cmd, void *a) \
{ \
 unsigned n = step_in(COUNT_##name); \
 void *result = old_##name(self, cmd, a); \
 say("redwood: store: #%u is for promise %p\n", n, (void *)self); \
 step_out(COUNT_##name, n, result != NULL); \
 return result; \
}
#define WRAPQ1(name) \
static void *(*old_##name)(id, SEL, void *); \
static void * \
new_##name(id self, SEL cmd, void *a) \
{ \
 entered[COUNT_##name]++; \
 void *result = old_##name(self, cmd, a); \
 returned[COUNT_##name]++; \
 return result; \
}
#define WRAPQ2(name) \
static void *(*old_##name)(id, SEL, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b) \
{ \
 entered[COUNT_##name]++; \
 void *result = old_##name(self, cmd, a, b); \
 returned[COUNT_##name]++; \
 return result; \
}
#define WRAPQ3(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c) \
{ \
 entered[COUNT_##name]++; \
 void *result = old_##name(self, cmd, a, b, c); \
 returned[COUNT_##name]++; \
 return result; \
}
#define FAIRPLAY_DEVICE_TYPE "REDWOOD-INJECTED-NOT-A-FAIRPLAY-DEVICE"
static CFStringRef fairplay_device_type;
static id (*old_fpDeviceType)(id, SEL);
static id
new_fpDeviceType(id self, SEL _cmd)
{
 (void)self;
 (void)_cmd;
 entered[COUNT_fpDeviceType]++;
 returned[COUNT_fpDeviceType]++;
 say("redwood: store: FairPlay device type answered INJECTED \"" FAIRPLAY_DEVICE_TYPE "\" "
     "(not measured; the real one blocks for ever, see src/inject_fairplay.c)\n");
 return (id)fairplay_device_type;
}
static id (*old_afterUpdating)(id, SEL, void *);
static id
new_afterUpdating(id self, SEL cmd, void *storefront)
{
 id account;
 (void)storefront;
 entered[COUNT_afterUpdating]++;
 account = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("ams_localiTunesAccount"));
 returned[COUNT_afterUpdating]++;
 say("redwood: store: _ams_localiTunesAccountAfterUpdatingStorefront: answered at once, %s\n",
     account != NULL ? "with the local account" : "with nothing");
 return account;
}
static void *(*old_bagValue)(id, SEL, void *);
static void *
new_bagValue(id self, SEL cmd, void *error)
{
 char frames[10 * 20 + 1] = "";
 void *result;
 char which[120];
 entered[COUNT_bagValue]++;
 frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
 result = old_bagValue(self, cmd, error);
 returned[COUNT_bagValue]++;
 {
  SEL key_sel = sel_registerName("key");
  id key = class_getInstanceMethod(object_getClass(self), key_sel) != NULL ?
      ((id (*)(id, SEL))objc_msgSend)(self, key_sel) : NULL;
  note_bagkey(key != NULL ? text_of(key, which, sizeof(which)) : "(no key)", result != NULL);
 }
 return result;
}
static void *(*old_resultBefore)(id, SEL, void *, void *);
static void *
new_resultBefore(id self, SEL cmd, void *date, void *error)
{
 char frames[10 * 20 + 1] = "";
 void *result;
 entered[COUNT_resultBefore]++;
 frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
 note_waiter((void *)self, frames, true);
 result = old_resultBefore(self, cmd, date, error);
 returned[COUNT_resultBefore]++;
 return result;
}
static void *(*old_finishFunnel)(id, SEL, void *, void *, void *);
static void *
new_finishFunnel(id self, SEL cmd, void *a, void *b, void *c)
{
 void *result;
 entered[COUNT_finishFunnel]++;
 note_finished((void *)self);
 result = old_finishFunnel(self, cmd, a, b, c);
 returned[COUNT_finishFunnel]++;
 return result;
}
WRAPQ2(finishResultError)
static void *(*old_thenWithBlock)(id, SEL, void *);
static void *
new_thenWithBlock(id self, SEL cmd, void *a)
{
 void *result;
 char frames[10 * 20 + 1] = "";
 entered[COUNT_thenWithBlock]++;
 frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
 note_waiter((void *)self, frames, false);
 result = old_thenWithBlock(self, cmd, a);
 returned[COUNT_thenWithBlock]++;
 return result;
}
static void *(*old_continueWithBlock)(id, SEL, void *);
static void *
new_continueWithBlock(id self, SEL cmd, void *a)
{
 void *result;
 char frames[10 * 20 + 1] = "";
 entered[COUNT_continueWithBlock]++;
 frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
 note_waiter((void *)self, frames, false);
 result = old_continueWithBlock(self, cmd, a);
 returned[COUNT_continueWithBlock]++;
 return result;
}
WRAP1(finishWithResult)
WRAP1(finishWithError)
WRAP3(addCookies)
WRAP3(addStorefrontHeader)
WRAP3(addAnisette)
WRAP3(prepareRequest)
WRAP4(addCookies4)
static struct replacement replacements[] = {
 { "ACAccount", "ams_storefront", false, (IMP)new_ams_storefront, (IMP *)&old_ams_storefront, false },
 { "ACAccount", "ams_copyStorefrontFromAccount:", false, (IMP)new_copy_storefront, (IMP *)&old_copy_storefront, false },
 { "AMSBagNetworkDataSource", "cachedStorefront", false, (IMP)new_cached_storefront, (IMP *)&old_cached_storefront, false },
 { "AMSBagFixedAccountProvider", "bagStorefrontForAccountMediaType:", false, (IMP)new_bag_storefront,
   (IMP *)&old_bag_storefront, false },
 { "AMSBagActiveAccountProvider", "bagStorefrontForAccountMediaType:", false, (IMP)new_bag_storefront_active,
   (IMP *)&old_bag_storefront_active, false },
 { "AMSBagActiveAccountProvider", "_bagAccountForAccountMediaType:mustContainStorefront:", false,
   (IMP)new_bag_account, (IMP *)&old_bag_account, false },
 { "AMSBagNetworkTask", "_performFetchWithAttemptedCount:account:storefront:", false, (IMP)new_perform_fetch,
   (IMP *)&old_perform_fetch, false },
 { "AMSBagNetworkTask", "_queryItemsForClientInfo:profile:profileVersion:cookieNames:account:storefront:", false,
   (IMP)new_query_items, (IMP *)&old_query_items, false },
 { "AMSURLSession", "performRequest:completion:", false, (IMP)new_perform_request, (IMP *)&old_perform_request, false },
 { "AMSURLSession", "performRequest:completionBlock:", false, (IMP)new_perform_request_block,
   (IMP *)&old_perform_request_block, false },
 { "AMSBagNetworkTask", "_createRequestWithQueryItems:", false, (IMP)new_create_request, (IMP *)&old_create_request,
   false },
 { "AMSBagNetworkTask", "URLSessionPromise", false, (IMP)new_session_promise, (IMP *)&old_session_promise, false },
 { "AMSURLSession", "dataTaskPromiseWithRequest:", false, (IMP)new_data_task_promise, (IMP *)&old_data_task_promise,
   false },
 { "AMSURLSession", "dataTaskWithRequest:", false, (IMP)new_data_task, (IMP *)&old_data_task, false },
 { "NSMutableURLRequest", "ams_addCookiesForAccount:clientInfo:bag:", false, (IMP)new_addCookies,
   (IMP *)&old_addCookies, false },
 { "NSMutableURLRequest", "ams_addStoreFrontHeaderForAccount:client:withBag:", false, (IMP)new_addStorefrontHeader,
   (IMP *)&old_addStorefrontHeader, false },
 { "NSMutableURLRequest", "ams_addAnisetteHeadersForAccount:type:bag:", false, (IMP)new_addAnisette,
   (IMP *)&old_addAnisette, false },
 { "AMSURLSession", "_prepareRequest:properties:error:", false, (IMP)new_prepareRequest,
   (IMP *)&old_prepareRequest, false },
 { "AMSMutablePromise", "finishWithResult:", false, (IMP)new_finishWithResult, (IMP *)&old_finishWithResult, false },
 { "AMSMutablePromise", "finishWithError:", false, (IMP)new_finishWithError, (IMP *)&old_finishWithError, false },
 { "AMSMutablePromise", "_finishWithResult:error:logDuplicateFinishes:", false, (IMP)new_finishFunnel,
   (IMP *)&old_finishFunnel, false },
 { "AMSMutablePromise", "finishWithResult:error:", false, (IMP)new_finishResultError,
   (IMP *)&old_finishResultError, false },
 { "AMSMutablePromise", "thenWithBlock:", false, (IMP)new_thenWithBlock, (IMP *)&old_thenWithBlock, false },
 { "AMSMutablePromise", "continueWithBlock:", false, (IMP)new_continueWithBlock, (IMP *)&old_continueWithBlock,
   false },
 { "ACAccountStore", "_ams_localiTunesAccountAfterUpdatingStorefront:", false, (IMP)new_afterUpdating,
   (IMP *)&old_afterUpdating, false },
 { "NSMutableURLRequest", "ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies:", false,
   (IMP)new_addCookiesBlocking, (IMP *)&old_addCookiesBlocking, false },
 { "ACAccountStore", "_updateStorefrontInLocalAccount:", false, (IMP)new_updateStorefront,
   (IMP *)&old_updateStorefront, false },
 { "JPReader", "processStream:signatureVerifier:options:error:", false, (IMP)new_processStream,
   (IMP *)&old_processStream, false },
 { "JEBrotliDecoder", "decompressStreamWithAvailableIn:nextIn:availableOut:nextOut:", false, (IMP)new_brotli,
   (IMP *)&old_brotli, false },
 { "NSNotificationCenter", "postNotificationName:object:userInfo:", false, (IMP)new_postName,
   (IMP *)&old_postName, false },
 { "NSNotificationCenter", "postNotification:", false, (IMP)new_postNotification,
   (IMP *)&old_postNotification, false },
 { "AMSUserAgent", "_sharedComponentFairPlayDeviceType", false, (IMP)new_fpDeviceType,
   (IMP *)&old_fpDeviceType, false },
 { "AMSBagValue", "valueWithError:", false, (IMP)new_bagValue, (IMP *)&old_bagValue, false },
 { "AMSPromise", "resultBeforeDate:error:", false, (IMP)new_resultBefore, (IMP *)&old_resultBefore, false },
 { "__NSURLSessionLocal", "dataTaskWithRequest:completionHandler:", false, (IMP)new_dataTaskLocal,
   (IMP *)&old_dataTaskLocal, false },
 { "__NSURLSessionLocal", "dataTaskWithRequest:", false, (IMP)new_dataTaskPlain,
   (IMP *)&old_dataTaskPlain, false },
 { "__NSURLSessionLocal", "dataTaskWithURL:completionHandler:", false, (IMP)new_dataTaskURL,
   (IMP *)&old_dataTaskURL, false },
 { "AMSURLSession", "dataTaskWithRequest:signpostID:completionHandler:", false, (IMP)new_dataTaskSignpost,
   (IMP *)&old_dataTaskSignpost, false },
 { "AMSURLSession", "dataTaskWithRequest:completionHandler:", false, (IMP)new_dataTaskAMS,
   (IMP *)&old_dataTaskAMS, false },
 { "NSError", "errorWithDomain:code:userInfo:", true, (IMP)new_errorWith, (IMP *)&old_errorWith, false },
 { "NSError", "initWithDomain:code:userInfo:", false, (IMP)new_errorInit, (IMP *)&old_errorInit, false },
 { "NSURLSessionTask", "resume", false, (IMP)new_resume_base, (IMP *)&old_resume_base, false },
 { "__NSCFLocalSessionTask", "resume", false, (IMP)new_resume_local, (IMP *)&old_resume_local, false },
};
static size_t
replacement_count(void)
{
 return sizeof(replacements) / sizeof(replacements[0]);
}
static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
 for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
  struct replacement *r = &replacements[i];
  Class cls;
  Method method;
  IMP original;
  if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
   continue;
  }
  SEL selector = sel_registerName(r->selector);
  Class super = class_getSuperclass(cls);
  method = r->class_method ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);
  if (method == NULL) {
   continue;
  }
  if (super != NULL && method == (r->class_method ? class_getClassMethod(super, selector) :
      class_getInstanceMethod(super, selector))) {
   r->done = true;
   say("redwood: store: %c[%s %s] is inherited, not this class's: left alone\n",
       r->class_method ? '+' : '-', r->class_name, r->selector);
   continue;
  }
  r->done = true;
  original = method_getImplementation(method);
  if (r->original != NULL && *r->original == NULL) {
   *r->original = original;
  }
  method_setImplementation(method, r->replacement);
  say("redwood: store: %c[%s %s] taken (was %#lx unslid)\n", r->class_method ? '+' : '-', r->class_name,
      r->selector, (uintptr_t)original - slide);
 }
}
__attribute__((constructor))
static void
redwood_store_init(void)
{
 const char *name = getprogname();
 size_t length = 0;
 if (name == NULL || (strcmp(name, "appstored") != 0 && strcmp(name, "AppStore") != 0)) {
  return;
 }
 slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
 watch_for_faults();
 storefront = CFStringCreateWithCString(NULL, STOREFRONT, kCFStringEncodingUTF8);
 fairplay_device_type = CFStringCreateWithCString(NULL, FAIRPLAY_DEVICE_TYPE, kCFStringEncodingUTF8);
 who = name;
 say("redwood: store(%s): ours is in, storefront to answer with is " STOREFRONT "\n", name);
 {
  pthread_t thread;
  if (pthread_create(&thread, NULL, late_report, NULL) == 0) {
   pthread_detach(thread);
  }
 }
 _dyld_register_func_for_add_image(image_added);
}
