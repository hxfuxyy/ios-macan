#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <math.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *);
static int level;
static uintptr_t cache_slide;
static bool suite_started, launched, geometry_ok = true;
static unsigned installed, rejected, scene_count, connected_count, window_count, load_count, layout_count;
static unsigned content_count, registration_count, render_count, gain_count;
static bool content_present, ordinary_backing;
static char backing_key;
static UIWindow *aperture_window;
static UIViewController *aperture_vc;
static id shape, probe_assertion;
static NSMutableArray *probe_objects;
static UIView *diagnostic_view;
static const CGRect footprint = {{125, 4}, {125, 110.0 / 3.0}};

typedef struct SBSystemApertureContainerRenderingConfiguration { NSInteger renderingStyle, cloningStyle; } SBSystemApertureContainerRenderingConfiguration;

static void ilog(const char *format, ...)
{
	char line[2048];
	va_list ap;
	int fd, n;
	memcpy(line, "redwood: island: ", 17);
	va_start(ap, format);
	n = vsnprintf(line + 17, sizeof(line) - 18, format, ap);
	va_end(ap);
	if (n < 0) return;
	size_t used = 17 + MIN((size_t)n, sizeof(line) - 19);
	line[used++] = '\n';
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	(void)write(fd >= 0 ? fd : 2, line, used);
	if (fd >= 0) close(fd);
}

static const char *clsname(id obj) { return obj ? object_getClassName(obj) : "nil"; }
static const char *str(id obj) { return [obj isKindOfClass:[NSString class]] ? [obj UTF8String] : clsname(obj); }

static bool signature(Method m, const char *ret, const char *args)
{
	char type[256];
	if (!m || method_getNumberOfArguments(m) != strlen(args) + 2) return false;
	method_getReturnType(m, type, sizeof(type));
	if (strcmp(type, ret)) return false;
	method_getArgumentType(m, 0, type, sizeof(type));
	if (strcmp(type, "@")) return false;
	method_getArgumentType(m, 1, type, sizeof(type));
	if (strcmp(type, ":")) return false;
	for (unsigned i = 0; args[i]; i++) {
		method_getArgumentType(m, i + 2, type, sizeof(type));
		if (type[0] != args[i] || type[1]) return false;
	}
	return true;
}

static bool uuid_matches(const void *address, const char *wanted)
{
	Dl_info info;
	if (!dladdr(address, &info) || !info.dli_fbase) return false;
	const struct mach_header_64 *mh = info.dli_fbase;
	if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64) return false;
	const struct load_command *lc = (const void *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		if (lc->cmdsize < sizeof(*lc)) return false;
		if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
			const struct uuid_command *u = (const void *)lc;
			char hex[33];
			for (unsigned j = 0; j < 16; j++) snprintf(hex + j * 2, 3, "%02x", u->uuid[j]);
			ilog("uuid image=%s actual=%s expected=%s match=%d", info.dli_fname, hex, wanted, !strcmp(hex, wanted));
			return !strcmp(hex, wanted);
		}
		lc = (const void *)((const char *)lc + lc->cmdsize);
	}
	return false;
}

struct hook {
	char kind;
	const char *class_name, *selector;
	uintptr_t expected;
	const char *ret, *args;
	IMP replacement;
	void **original;
};

static void install(struct hook *h)
{
	if (*h->original) return;
	Class c = objc_getClass(h->class_name);
	SEL s = sel_registerName(h->selector);
	Method m = h->kind == '+' ? class_getClassMethod(c, s) : class_getInstanceMethod(c, s);
	IMP old = m ? method_getImplementation(m) : NULL;
	bool valid = old && (uintptr_t)old == h->expected + cache_slide && signature(m, h->ret, h->args);
	ilog("hook %c[%s %s] actual=%#lx expected=%#lx type=%s %s", h->kind, h->class_name, h->selector,
	    old ? (uintptr_t)old - cache_slide : 0, h->expected, m ? method_getTypeEncoding(m) : "missing",
	    valid ? "installed" : "REJECTED");
	if (!valid) { rejected++; return; }
	*h->original = (void *)old;
	Class target = h->kind == '+' ? object_getClass(c) : c;
	if (!class_addMethod(target, s, h->replacement, method_getTypeEncoding(m))) method_setImplementation(m, h->replacement);
	installed++;
}

static bool contract(const char *name, const char *selector, uintptr_t address, const char *ret, const char *args)
{
	Method m = class_getInstanceMethod(objc_getClass(name), sel_registerName(selector));
	bool ok = m && (uintptr_t)method_getImplementation(m) == address + cache_slide && signature(m, ret, args);
	ilog("contract -[%s %s] type=%s %s", name, selector, m ? method_getTypeEncoding(m) : "missing", ok ? "ok" : "REJECTED");
	return ok;
}

static id object_value(id obj, const char *name)
{
	SEL sel = sel_registerName(name);
	Method m = obj ? class_getInstanceMethod(object_getClass(obj), sel) : NULL;
	return signature(m, "@", "") ? ((id (*)(id, SEL))objc_msgSend)(obj, sel) : nil;
}

static CGRect rect_value(id obj, const char *name)
{
	SEL sel = sel_registerName(name);
	Method m = obj ? class_getInstanceMethod(object_getClass(obj), sel) : NULL;
	return signature(m, @encode(CGRect), "") ? ((CGRect (*)(id, SEL))objc_msgSend)(obj, sel) : CGRectZero;
}

static NSInteger (*original_subtype)(void);
static bool (*original_jindo)(void), (*original_enabled)(void);
static NSInteger subtype_hook(void)
{
	NSInteger original = original_subtype();
	NSInteger effective = level >= 2 ? 2556 : original;
	static unsigned count;
	if (count++ < 4) ilog("eligibility originalSubtype=%ld effectiveSubtype=%ld", (long)original, (long)effective);
	return effective;
}
static bool jindo_hook(void)
{
	bool original = original_jindo();
	bool effective = original || level >= 2;
	static unsigned count;
	if (count++ < 4) ilog("jindo original=%d effective=%d forced=%d", original, effective, !original && effective);
	return effective;
}
static bool enabled_hook(void)
{
	bool original = original_enabled();
	static unsigned count;
	if (count++ < 4) ilog("SBUIIsSystemApertureEnabled=%d unmodified=1", original);
	return original;
}

static void function_hooks(void)
{
	void (*hook)(void *, void *, void **) = dlsym(RTLD_DEFAULT, "MSHookFunction");
	if (!hook) {
		const char *paths[] = {"/var/jb/usr/lib/libsubstrate.dylib", "/var/jb/usr/lib/libellekit.dylib"};
		for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]) && !hook; i++) {
			void *lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
			if (lib) hook = dlsym(lib, "MSHookFunction");
			ilog("engine path=%s loaded=%d MSHookFunction=%p", paths[i], lib != NULL, hook);
		}
	}
	if (!hook) { ilog("functions FAILED reason=MSHookFunction-unavailable eligibility-unchanged"); rejected += 3; return; }
	ilog("engine MSHookFunction=%p", hook);
	struct { const char *name; uintptr_t address; void *replacement; void **original; } funcs[] = {
		{"SBFEffectiveArtworkSubtype", 0x18e5f03fc, subtype_hook, (void **)&original_subtype},
		{"SBSEnableJindo", 0x19153b14c, jindo_hook, (void **)&original_jindo},
		{"SBUIIsSystemApertureEnabled", 0x1a340939c, enabled_hook, (void **)&original_enabled}
	};
	for (unsigned i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
		if (*funcs[i].original) continue;
		void *p = dlsym(RTLD_DEFAULT, funcs[i].name);
		if (p != (void *)(funcs[i].address + cache_slide)) {
			ilog("function %s REJECTED actual=%p expected=%p", funcs[i].name, p, (void *)(funcs[i].address + cache_slide));
			rejected++; continue;
		}
		hook(p, funcs[i].replacement, funcs[i].original);
		ilog("function %s trampoline=%p %s", funcs[i].name, *funcs[i].original, *funcs[i].original ? "installed" : "FAILED");
		if (*funcs[i].original) installed++; else rejected++;
	}
}

static void install_suite(void);
static void diagnostics(void);
static void try_probe(void);
static void update_backing(UIView *view);

static id make_shape(void)
{
	if (!shape) {
		Method m = class_getInstanceMethod(objc_getClass("UISDisplaySingleRectShape"), sel_registerName("initWithRect:"));
		char ret[16], arg[128];
		if (!m) { ilog("shape FAILED reason=missing-initWithRect"); return nil; }
		method_getReturnType(m, ret, sizeof(ret));
		method_getArgumentType(m, 2, arg, sizeof(arg));
		if ((uintptr_t)method_getImplementation(m) != 0x193436944 + cache_slide ||
		    method_getNumberOfArguments(m) != 3 || strcmp(ret, "@") || strcmp(arg, @encode(CGRect))) {
			ilog("shape FAILED reason=initWithRect-contract type=%s", method_getTypeEncoding(m)); return nil;
		}
		shape = ((id (*)(id, SEL, CGRect))objc_msgSend)([objc_getClass("UISDisplaySingleRectShape") alloc],
		    sel_registerName("initWithRect:"), footprint);
		ilog("shape created=%p rect={125,4,125,36.667}", shape);
	}
	return shape;
}

static id (*original_shape)(id, SEL);
static id shape_hook(id self, SEL cmd)
{
	install_suite();
	id original = original_shape(self, cmd);
	id result = level >= 2 && geometry_ok ? make_shape() : original;
	static unsigned count;
	if (count++ < 3) ilog("shape original=%s rect=%s effective=%s", clsname(original),
	    [NSStringFromCGRect(rect_value(original, "rect")) UTF8String], [NSStringFromCGRect(rect_value(result, "rect")) UTF8String]);
	return result ?: original;
}

static CGSize (*original_sensor)(id, SEL);
static CGSize sensor_hook(id self, SEL cmd)
{
	CGSize old = original_sensor(self, cmd), result = level >= 2 && geometry_ok ? footprint.size : old;
	static unsigned count;
	if (count++ < 2) ilog("metrics sensor original=%s effective=%s", [NSStringFromCGSize(old) UTF8String], [NSStringFromCGSize(result) UTF8String]);
	return result;
}
static NSDirectionalEdgeInsets (*original_insets)(id, SEL);
static NSDirectionalEdgeInsets insets_hook(id self, SEL cmd)
{
	NSDirectionalEdgeInsets old = original_insets(self, cmd), result = old;
	if (level >= 2 && geometry_ok) result.top = footprint.origin.y;
	static unsigned count;
	if (count++ < 2) ilog("metrics insets original={%.3f,%.3f,%.3f,%.3f} effective={%.3f,%.3f,%.3f,%.3f}", old.top, old.leading, old.bottom, old.trailing, result.top, result.leading, result.bottom, result.trailing);
	return result;
}
static CGRect (*original_gap)(id, SEL);
static CGRect gap_hook(id self, SEL cmd)
{
	CGRect old = original_gap(self, cmd), result = old;
	if (level >= 2 && geometry_ok) result = CGRectMake(204.0 + 1.0 / 3.0, footprint.origin.y, 9, footprint.size.height);
	static unsigned count;
	if (count++ < 2) ilog("metrics gap original=%s effective=%s", [NSStringFromCGRect(old) UTF8String], [NSStringFromCGRect(result) UTF8String]);
	return result;
}
static id (*original_exclusion)(id, SEL);
static id exclusion_hook(id self, SEL cmd)
{
	id old = original_exclusion(self, cmd);
	if (level < 2 || !geometry_ok || self != [UIScreen mainScreen]) return old;
	if (CGRectEqualToRect(rect_value(old, "rect"), footprint)) return old;
	static unsigned count;
	if (count++ < 3) ilog("exclusion stale-main-context original=%s fallback={125,4,125,36.667}", [NSStringFromCGRect(rect_value(old, "rect")) UTF8String]);
	return make_shape() ?: old;
}
static CGFloat (*original_height)(id, SEL);
static CGFloat height_hook(id self, SEL cmd)
{
	CGFloat old = original_height(self, cmd);
	static unsigned count;
	CGFloat result = level >= 2 && geometry_ok ? 44 : old;
	if (count++ < 3) ilog("statusbar class=%s heightOriginal=%.3f effective=%.3f", class_getName(self), old, result);
	return result;
}
static Class (*original_provider)(id, SEL, id, id);
static Class provider_hook(id self, SEL cmd, id screen, id info)
{
	Class result = original_provider(self, cmd, screen, info);
	static unsigned count;
	if (count++ < 6) ilog("statusbar provider=%s exclusion=%s", class_getName(result),
	    [NSStringFromCGRect(rect_value(object_value(screen, "_exclusionArea"), "rect")) UTF8String]);
	return result;
}
static CGFloat (*original_width)(id, SEL);
static CGFloat width_hook(id self, SEL cmd)
{
	CGFloat result = original_width(self, cmd);
	static unsigned count;
	if (count++ < 3) ilog("statusbar provider=%s effectiveWidth=%.3f", clsname(self), result);
	return result;
}

static void (*original_configure)(id, SEL, id, id);
static void configure_hook(id self, SEL cmd, id scene, id context)
{
	install_suite();
	ilog("mainScene configure entered scene=%s context=%s", clsname(scene), clsname(context));
	original_configure(self, cmd, scene, context);
	ilog("mainScene configure completed");
}
static void (*original_high)(id, SEL, id), (*original_curtain)(id, SEL, id);
static void high_hook(id self, SEL cmd, id config)
{
	ilog("sceneFactory normal entered subtype=%ld jindo=%d", original_subtype ? (long)subtype_hook() : -1L,
	    original_jindo ? jindo_hook() : -1);
	original_high(self, cmd, config);
	ilog("sceneFactory normal returned scenes=%u", scene_count);
}
static void curtain_hook(id self, SEL cmd, id config)
{
	ilog("sceneFactory curtain entered");
	original_curtain(self, cmd, config);
	ilog("sceneFactory curtain returned scenes=%u", scene_count);
}
static id (*original_scene)(id, SEL, id, id, id, float);
static id scene_hook(id self, SEL cmd, id identifier, id spec, id config, float at_level)
{
	id result = original_scene(self, cmd, identifier, spec, config, at_level);
	if (result) scene_count++;
	ilog("scene identifier=%s object=%p class=%s level=%.1f", str(identifier), result, clsname(result), at_level);
	return result;
}
static void (*original_connect)(id, SEL, id, id, id);
static void connect_hook(id self, SEL cmd, id scene, id session, id options)
{
	ilog("connect entered scene=%s role=%s", clsname(scene), str(object_value(session, "role")));
	original_connect(self, cmd, scene, session, options);
	connected_count++;
	ilog("connect completed windows=%u", window_count);
}
static void (*original_window)(id, SEL, id);
static void window_hook(id self, SEL cmd, id scene)
{
	ilog("window entered scene=%s", clsname(scene));
	original_window(self, cmd, scene);
	NSArray *windows = object_value(scene, "windows");
	for (UIWindow *window in windows) {
		if ([window isKindOfClass:objc_getClass("SBSystemApertureWindow")]) {
			if (aperture_window != window) { [aperture_window release]; aperture_window = [window retain]; }
			window_count++;
			ilog("window class=%s root=%s hidden=%d alpha=%.3f frame=%s", clsname(window),
			    clsname(window.rootViewController), window.hidden, window.alpha, [NSStringFromCGRect(window.frame) UTF8String]);
		}
	}
	ilog("window completed found=%d", aperture_window != nil);
}
static id (*original_vcinit)(id, SEL, id);
static id vcinit_hook(id self, SEL cmd, id scene)
{
	id result = original_vcinit(self, cmd, scene);
	ilog("viewController init scene=%s result=%p", clsname(scene), result);
	return result;
}
static void (*original_load)(id, SEL), (*original_layout)(id, SEL);
static void load_hook(id self, SEL cmd)
{
	ilog("viewDidLoad entered");
	original_load(self, cmd);
	load_count++;
	if (aperture_vc != self) { [aperture_vc release]; aperture_vc = [self retain]; }
	ilog("viewDidLoad completed view=%s", clsname([(UIViewController *)self view]));
}
static void layout_hook(id self, SEL cmd)
{
	original_layout(self, cmd);
	if (layout_count++ < 3) {
		UIView *view = [(UIViewController *)self view];
		ilog("layout frame=%s minimumFrame=%s safeArea=%s", [NSStringFromCGRect(view.frame) UTF8String],
		    [NSStringFromCGRect(rect_value(self, "minimumSensorRegionFrame")) UTF8String], [NSStringFromUIEdgeInsets(view.safeAreaInsets) UTF8String]);
	}
}
static void (*original_content)(id, SEL, id, BOOL);
static void content_hook(id self, SEL cmd, id vc, BOOL present)
{
	original_content(self, cmd, vc, present);
	content_present = present;
	content_count++;
	ilog("content containsAnyContent=%d changes=%u", present, content_count);
}
static id (*original_register)(id, SEL, id);
static id register_hook(id self, SEL cmd, id element)
{
	id result = original_register(self, cmd, element);
	registration_count++;
	ilog("register element=%s assertion=%p count=%u", clsname(element), result, registration_count);
	return result;
}
static void (*original_render)(id, SEL, SBSystemApertureContainerRenderingConfiguration);
static void render_hook(id self, SEL cmd, SBSystemApertureContainerRenderingConfiguration config)
{
	original_render(self, cmd, config);
	UIView *backing = objc_getAssociatedObject(self, &backing_key);
	if (backing) backing.hidden = config.renderingStyle != 0;
	if (render_count++ < 20) {
		UIView *view = self;
		ilog("render style=%ld cloning=%ld container=%p frame=%s hidden=%d alpha=%.3f masks=%d", (long)config.renderingStyle,
		    (long)config.cloningStyle, self, [NSStringFromCGRect(view.frame) UTF8String], view.hidden, view.alpha, view.layer.masksToBounds);
	}
}
static void (*original_gain)(id, SEL, SBSystemApertureContainerRenderingConfiguration);
static void gain_hook(id self, SEL cmd, SBSystemApertureContainerRenderingConfiguration config)
{
	original_gain(self, cmd, config);
	if (gain_count++ < 12) ilog("gain configuration style=%ld cloning=%ld view=%p", (long)config.renderingStyle, (long)config.cloningStyle, self);
}
static void (*original_request)(id, SEL, id, id, id);
static void request_hook(id self, SEL cmd, id server, id client, id request)
{
	ilog("clientScene request server=%s client=%s request=%s", clsname(server), clsname(client), str(request));
	original_request(self, cmd, server, client, request);
	ilog("clientScene request completed");
}

static void (*original_container_layout)(id, SEL);
static void container_layout_hook(id self, SEL cmd)
{
	original_container_layout(self, cmd);
	if (ordinary_backing) update_backing(self);
}
static id (*original_manager_init)(id, SEL, id), (*original_manager_register)(id, SEL, id);
static id manager_init_hook(id self, SEL cmd, id authority)
{
	id result = original_manager_init(self, cmd, authority);
	ilog("manager init authority=%s result=%p", clsname(authority), result);
	return result;
}
static id manager_register_hook(id self, SEL cmd, id element)
{
	id result = original_manager_register(self, cmd, element);
	ilog("manager register element=%s assertion=%p", clsname(element), result);
	return result;
}
static id (*original_restrict_inert)(id, SEL, id), (*original_restrict_default)(id, SEL, id);
static id restrict_inert_hook(id self, SEL cmd, id reason)
{
	id result = original_restrict_inert(self, cmd, reason);
	ilog("suppression restrict=inert reason=%s assertion=%p", str(reason), result);
	return result;
}
static id restrict_default_hook(id self, SEL cmd, id reason)
{
	id result = original_restrict_default(self, cmd, reason);
	ilog("suppression restrict=default reason=%s assertion=%p", str(reason), result);
	return result;
}

#define H(kind, cls, sel, addr, ret, args, fn, orig) {kind, cls, sel, addr, ret, args, (IMP)fn, (void **)&orig}
static struct hook suite[] = {
	H('-', "SAUISystemApertureManager", "initWithElementAuthority:", 0x20bda6ddc, "@", "@", manager_init_hook, original_manager_init),
	H('-', "SAUISystemApertureManager", "registerElement:", 0x20bda6f04, "@", "@", manager_register_hook, original_manager_register),
	H('-', "SBSystemApertureController", "restrictSystemApertureToInertWithReason:", 0x1c50c64b4, "@", "@", restrict_inert_hook, original_restrict_inert),
	H('-', "SBSystemApertureController", "restrictSystemApertureToDefaultLayoutWithReason:", 0x1c50c64ac, "@", "@", restrict_default_hook, original_restrict_default),
	H('-', "SBSystemApertureContainerView", "layoutSubviews", 0x1c4b3f250, "v", "", container_layout_hook, original_container_layout),
	H('-', "SBUISystemApertureLayoutMetrics", "sensorRegionSize", 0x1a33f7598, @encode(CGSize), "", sensor_hook, original_sensor),
	H('-', "SBUISystemApertureLayoutMetrics", "minimumScreenEdgeInsets", 0x1a33f7578, @encode(NSDirectionalEdgeInsets), "", insets_hook, original_insets),
	H('-', "SBUISystemApertureLayoutMetrics", "interSensorRegionInWindowSpace", 0x1a33f75d0, @encode(CGRect), "", gap_hook, original_gap),
	H('-', "UIScreen", "_exclusionArea", 0x189252460, "@", "", exclusion_hook, original_exclusion),
	H('+', "_UIStatusBarVisualProvider_DynamicSplit", "height", 0x1899eff18, "d", "", height_hook, original_height),
	H('+', "_UIStatusBarVisualProvider_Phone", "visualProviderSubclassForScreen:visualProviderInfo:", 0x189aab9b0, "#", "@@", provider_hook, original_provider),
	H('-', "_UIStatusBarVisualProvider_DynamicSplit", "effectiveTargetDisplayWidth", 0x1899f01c8, "d", "", width_hook, original_width),
	H('-', "SBSystemApertureController", "createHighLevelWindowSceneWithDisplayConfiguration:", 0x1c50c5a3c, "v", "@", high_hook, original_high),
	H('-', "SBSystemApertureController", "createSuperHighLevelCurtainWindowSceneWithDisplayConfiguration:", 0x1c50c5ac8, "v", "@", curtain_hook, original_curtain),
	H('-', "SBSystemApertureController", "_createHighLevelSystemApertureSceneWithIdentifier:sceneSpecificiation:displayConfiguration:atLevel:", 0x1c50c5b54, "@", "@@@f", scene_hook, original_scene),
	H('-', "SBSystemApertureWindowSceneDelegate", "scene:willConnectToSession:options:", 0x1c4b3dadc, "v", "@@@", connect_hook, original_connect),
	H('-', "SBSystemApertureController", "createHighLevelSystemApertureWindowWithWindowScene:", 0x1c50c5e6c, "v", "@", window_hook, original_window),
	H('-', "SBSystemApertureViewController", "initWithWindowScene:", 0x1c4dcdd1c, "@", "@", vcinit_hook, original_vcinit),
	H('-', "SBSystemApertureViewController", "viewDidLoad", 0x1c4dd015c, "v", "", load_hook, original_load),
	H('-', "SBSystemApertureViewController", "viewWillLayoutSubviews", 0x1c4dd0684, "v", "", layout_hook, original_layout),
	H('-', "SBSystemApertureController", "systemApertureViewController:containsAnyContent:", 0x1c50c6730, "v", "@B", content_hook, original_content),
	H('-', "SBSystemApertureController", "registerElement:", 0x1c50c63cc, "@", "@", register_hook, original_register),
	H('-', "SBSystemApertureContainerView", "setRenderingConfiguration:", 0x1c4b3ed10, "v", "S", render_hook, original_render),
	H('-', "_SBSystemApertureGainMapView", "setRenderingConfiguration:", 0x1c4b41fc8, "v", "S", gain_hook, original_gain),
	H('-', "SBSystemApertureSceneHoster", "sceneCreationRequestServer:didReceiveRequestForSystemApertureSceneWithClientIdentity:requestIdentifier:", 0x1c50853dc, "v", "@@@", request_hook, original_request)
};

static void install_suite(void)
{
	if (suite_started) return;
	suite_started = true;
	ilog("install lazy level=%d", level);
	function_hooks();
	for (unsigned i = 0; i < sizeof(suite) / sizeof(suite[0]); i++) {
		if (*suite[i].original) continue;
		if (!strcmp(suite[i].args, "S")) {
			Method m = class_getInstanceMethod(objc_getClass(suite[i].class_name), sel_registerName(suite[i].selector));
			char arg[128], ret[16];
			if (!m) { ilog("hook %s renderingConfiguration REJECTED missing", suite[i].class_name); rejected++; continue; }
			method_getArgumentType(m, 2, arg, sizeof(arg));
			method_getReturnType(m, ret, sizeof(ret));
			if (method_getNumberOfArguments(m) != 3 || strcmp(ret, "v") ||
			    strcmp(arg, @encode(SBSystemApertureContainerRenderingConfiguration)) ||
			    (uintptr_t)method_getImplementation(m) != suite[i].expected + cache_slide) {
				ilog("hook %s renderingConfiguration REJECTED type=%s", suite[i].class_name, method_getTypeEncoding(m)); rejected++; continue;
			}
			*suite[i].original = (void *)method_getImplementation(m);
			method_setImplementation(m, suite[i].replacement);
			installed++;
			ilog("hook %s renderingConfiguration installed type=%s", suite[i].class_name, method_getTypeEncoding(m));
		} else install(&suite[i]);
	}
	ilog("hooks process=SpringBoard installed=%u rejected=%u", installed, rejected);
}

static unsigned long long integer_value(id obj, const char *name, bool *known)
{
	SEL sel = sel_registerName(name);
	Method m = obj ? class_getInstanceMethod(object_getClass(obj), sel) : NULL;
	*known = true;
	if (signature(m, "Q", "")) return ((unsigned long long (*)(id, SEL))objc_msgSend)(obj, sel);
	if (signature(m, "I", "")) return ((unsigned int (*)(id, SEL))objc_msgSend)(obj, sel);
	if (signature(m, "B", "")) return ((BOOL (*)(id, SEL))objc_msgSend)(obj, sel);
	*known = false;
	return 0;
}

static void dump_layer(CALayer *layer, unsigned depth, unsigned *budget)
{
	if (!layer || !*budget || depth > 5) return;
	(*budget)--;
	bool known;
	unsigned long long mask = integer_value(layer, "disableUpdateMask", &known);
	ilog("layer depth=%u class=%s frame=%s hidden=%d opacity=%.3f masks=%d renderMode=%s disableUpdateMask=%s%llu",
	    depth, clsname(layer), [NSStringFromCGRect(layer.frame) UTF8String], layer.hidden, layer.opacity,
	    layer.masksToBounds, str(object_value(layer, "renderMode")), known ? "" : "unavailable/", mask);
	for (CALayer *child in layer.sublayers) dump_layer(child, depth + 1, budget);
}
static void dump_view(UIView *view, unsigned depth, unsigned *budget)
{
	if (!view || !*budget || depth > 9) return;
	(*budget)--;
	ilog("view depth=%u class=%s frame=%s hidden=%d alpha=%.3f text=%s", depth, clsname(view),
	    [NSStringFromCGRect(view.frame) UTF8String], view.hidden, view.alpha,
	    [view isKindOfClass:[UILabel class]] ? [((UILabel *)view).text UTF8String] : "-");
	if (strstr(clsname(view), "GainMap") || strstr(clsname(view), "Curtain")) {
		unsigned layers = 12;
		dump_layer(view.layer, 0, &layers);
	}
	for (UIView *child in view.subviews) dump_view(child, depth + 1, budget);
}

static NSString *short_text(id value);
static void activity_dump(void)
{
	static bool listed;
	const char *names[] = {"SBActivityManager", "SBActivitySystemApertureElementObserver", "SBActivityProvidedContentElement",
	    "SBActivityItem", "SBActivityObserver"};
	for (unsigned c = 0; c < sizeof(names) / sizeof(names[0]) && !listed; c++) {
		Class cls = objc_getClass(names[c]);
		if (!cls) { ilog("activity class %s MISSING", names[c]); continue; }
		unsigned count = 0;
		Method *methods = class_copyMethodList(cls, &count);
		for (unsigned i = 0; i < count && i < 120; i++) {
			ilog("activity method %s -%s %s", names[c], sel_getName(method_getName(methods[i])), method_getTypeEncoding(methods[i]));
		}
		free(methods);
		methods = class_copyMethodList(object_getClass(cls), &count);
		for (unsigned i = 0; i < count && i < 40; i++) {
			ilog("activity method %s +%s %s", names[c], sel_getName(method_getName(methods[i])), method_getTypeEncoding(methods[i]));
		}
		free(methods);
		Ivar *ivars = class_copyIvarList(cls, &count);
		for (unsigned i = 0; i < count; i++) ilog("activity ivar %s %s %s", names[c], ivar_getName(ivars[i]), ivar_getTypeEncoding(ivars[i]));
		free(ivars);
	}
	listed = true;
	Class manager_class = objc_getClass("SBActivityManager");
	id manager = nil;
	if (manager_class && class_respondsToSelector(object_getClass(manager_class), sel_registerName("sharedInstance"))) {
		manager = ((id (*)(id, SEL))objc_msgSend)(manager_class, sel_registerName("sharedInstance"));
	}
	ilog("activity manager=%p", manager);
	if (!manager) return;
	Ivar observers = class_getInstanceVariable(manager_class, "_observers");
	for (id observer in observers ? (id<NSFastEnumeration>)object_getIvar(manager, observers) : @[]) {
		unsigned n = 0;
		Ivar *list = class_copyIvarList(object_getClass(observer), &n);
		for (unsigned i = 0; i < n; i++) {
			const char *type = ivar_getTypeEncoding(list[i]);
			if (type && type[0] == '@') ilog("activity observer.%s = %s", ivar_getName(list[i]), short_text(object_getIvar(observer, list[i])).UTF8String);
		}
		free(list);
		Ivar handles = class_getInstanceVariable(object_getClass(observer), "_sceneHandleByActivityIdentifier");
		NSDictionary *byId = handles ? object_getIvar(observer, handles) : nil;
		for (id key in byId) {
			id handle = byId[key];
			for (Class c = object_getClass(handle); c && c != [NSObject class]; c = class_getSuperclass(c)) {
				Ivar *hl = class_copyIvarList(c, &n);
				for (unsigned i = 0; i < n; i++) {
					const char *type = ivar_getTypeEncoding(hl[i]);
					if (type && type[0] == '@') {
						ilog("activity handle %s.%s = %s", class_getName(c), ivar_getName(hl[i]), short_text(object_getIvar(handle, hl[i])).UTF8String);
					} else if (type && (type[0] == 'B' || type[0] == 'c' || type[0] == 'q' || type[0] == 'Q')) {
						long long v = type[0] == 'B' || type[0] == 'c' ? *(signed char *)((uint8_t *)(__bridge void *)handle + ivar_getOffset(hl[i])) :
						    *(long long *)((uint8_t *)(__bridge void *)handle + ivar_getOffset(hl[i]));
						ilog("activity handle %s.%s = %lld", class_getName(c), ivar_getName(hl[i]), v);
					}
				}
				free(hl);
			}
		}
		for (id key in byId) {
			Ivar si = class_getInstanceVariable(object_getClass(byId[key]), "_scene");
			id scene = si ? object_getIvar(byId[key], si) : nil;
			const char *sels[] = {"contentState", "isActive", "isValid", "clientProcess", "clientHandle", "settings", "clientSettings",
			    "layerManager", "uiPresentationManager"};
			for (unsigned k = 0; scene && k < sizeof(sels) / sizeof(sels[0]); k++) {
				SEL sel = sel_registerName(sels[k]);
				Method m = class_getInstanceMethod(object_getClass(scene), sel);
				if (!m) { ilog("activity scene.%s missing", sels[k]); continue; }
				char ret[16] = "";
				method_getReturnType(m, ret, sizeof(ret));
				if (ret[0] == '@') {
					ilog("activity scene.%s = %s", sels[k], short_text(((id (*)(id, SEL))objc_msgSend)(scene, sel)).UTF8String);
				} else {
					ilog("activity scene.%s = %lld", sels[k], (long long)((long (*)(id, SEL))objc_msgSend)(scene, sel));
				}
			}
		}
		Ivar elements = class_getInstanceVariable(object_getClass(observer), "_elementByActivityIdentifier");
		NSDictionary *els = elements ? object_getIvar(observer, elements) : nil;
		for (id key in els) {
			NSString *text = [[els[key] description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
			for (NSUInteger at = 0; at < text.length && at < 3000; at += 900) {
				ilog("activity element %s", [text substringWithRange:NSMakeRange(at, MIN(900, text.length - at))].UTF8String);
			}
		}
	}
	unsigned count = 0;
	Ivar *ivars = class_copyIvarList(manager_class, &count);
	for (unsigned i = 0; i < count; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);
		if (type && type[0] == '@') {
			id value = object_getIvar(manager, ivars[i]);
			NSString *text = value ? [[value description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "] : @"nil";
			if (text.length > 600) text = [text substringToIndex:600];
			ilog("activity manager.%s = %s", ivar_getName(ivars[i]), text.UTF8String);
		} else if (type && (type[0] == 'B' || type[0] == 'c')) {
			ilog("activity manager.%s = %d", ivar_getName(ivars[i]), *(signed char *)((uint8_t *)(__bridge void *)manager + ivar_getOffset(ivars[i])));
		}
	}
	free(ivars);
}

static void diagnostics(void)
{
	if (!launched) { ilog("diagnostics deferred reason=not-launched"); return; }
	activity_dump();
	UIScreen *screen = [UIScreen mainScreen];
	CGRect bounds = screen.bounds, native = screen.nativeBounds;
	geometry_ok = fabs(bounds.size.width - 375) < .01 && fabs(bounds.size.height - 812) < .01 && fabs(screen.scale - 3) < .01;
	CFTypeRef (*mg)(CFStringRef) = dlsym(RTLD_DEFAULT, "MGCopyAnswer");
	CFTypeRef home = mg && (uintptr_t)mg == 0x19c527bd4 + cache_slide ? mg(CFSTR("JwLB44/jEB8aFDpXQ16Tuw")) : NULL;
	int home_button = -1;
	if (home && CFGetTypeID(home) == CFNumberGetTypeID()) CFNumberGetValue(home, kCFNumberIntType, &home_button);
	if (home) CFRelease(home);
	ilog("baseline bounds=%.0fx%.0f native=%.0fx%.0f scale=%.0f homeButton=%d canvas=%s", bounds.size.width, bounds.size.height,
	    native.size.width, native.size.height, screen.scale, home_button, geometry_ok ? "ok" : "MISMATCH");
	NSInteger subtype = original_subtype ? subtype_hook() : -1;
	int jindo = original_jindo ? jindo_hook() : -1;
	int enabled = original_enabled ? enabled_hook() : -1;
	ilog("gates subtype=%ld jindo=%d SBUIEnabled=%d", (long)subtype, jindo, enabled);
	UIView *view = aperture_vc.isViewLoaded ? aperture_vc.view : nil;
	UIWindow *main_window = nil;
	for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
		if (![scene isKindOfClass:[UIWindowScene class]] || ((UIWindowScene *)scene).screen != screen) continue;
		for (UIWindow *candidate in ((UIWindowScene *)scene).windows) {
			if (candidate == aperture_window || candidate.hidden || !candidate.rootViewController ||
			    !CGRectEqualToRect(candidate.bounds, bounds)) continue;
			if (!main_window || candidate.isKeyWindow) main_window = candidate;
		}
	}
	UIEdgeInsets safe = main_window ? main_window.safeAreaInsets : UIEdgeInsetsZero;
	id exclusion = object_value(screen, "_exclusionArea");
	ilog("geometry minimumFrame=%s screenExclusion=%s safeTop=%.3f safeBottom=%.3f statusBarFrame=%s",
	    [NSStringFromCGRect(rect_value(aperture_vc, "minimumSensorRegionFrame")) UTF8String],
	    [NSStringFromCGRect(rect_value(exclusion, "rect")) UTF8String], safe.top, safe.bottom,
	    [NSStringFromCGRect(main_window.windowScene.statusBarManager.statusBarFrame) UTF8String]);
	ilog("safeArea window=%s root=%s top=%.3f bottom=%.3f expected=44/34", clsname(main_window), clsname(main_window.rootViewController), safe.top, safe.bottom);
	unsigned budget = 90;
	dump_view(view, 0, &budget);
	ilog("summary scenes=%u connected=%u windows=%u viewDidLoad=%u layouts=%u registrations=%u contentChanges=%u containsAnyContent=%d renders=%u gains=%u probe=%d hooks=%u rejected=%u",
	    scene_count, connected_count, window_count, load_count, layout_count, registration_count, content_count,
	    content_present, render_count, gain_count, probe_assertion != nil, installed, rejected);
	const char *failure = !geometry_ok ? "canvas" : !original_subtype || !original_jindo ? "function-hooks" : !enabled ? "eligibility" :
	    !scene_count ? "scene-factory" : !connected_count ? "scene-connection" : !aperture_window ? "window" :
	    !load_count ? "viewDidLoad" : !layout_count ? "layout" : level == 3 && !probe_assertion ? "probe-registration" :
	    level == 3 && !content_present ? "content-or-suppression" :
	    level == 3 && !render_count ? "render-configuration" : "none-observed-check-panel";
	ilog("firstFailure=%s", failure);
}

static bool probe_contracts(void)
{
	struct { const char *c, *s; uintptr_t a; const char *r, *args; } checks[] = {
		{"SpringBoard", "systemApertureControllerForMainDisplay", 0x1c4acb9c0, "@", ""},
		{"_SBSystemApertureElementIdentification", "initWithClientIdentifier:elementIdentifier:", 0x1c4ddeeac, "@", "@@"},
		{"SBUISystemApertureCustomContentProvider", "initWithView:", 0x1a33cc9c4, "@", "@"},
		{"SBUISystemApertureContentProvider", "setLeadingContentViewProvider:", 0x1a33aded0, "v", "@"},
		{"SBUISystemApertureContentProvider", "setTrailingContentViewProvider:", 0x1a33adee4, "v", "@"},
		{"SBSystemApertureProvidedContentElement", "initWithIdentifier:contentProvider:", 0x1c4fc102c, "@", "@@"},
		{"SBSystemApertureProvidedContentElement", "setMinimumSupportedLayoutMode:", 0x1c4fc28fc, "v", "q"},
		{"SBSystemApertureProvidedContentElement", "setMaximumSupportedLayoutMode:", 0x1c4fc290c, "v", "q"},
		{"SBSystemApertureProvidedContentElement", "setPreferredLayoutMode:", 0x1c4fc291c, "v", "q"}
	};
	bool ok = original_register != NULL;
	for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		if (!contract(checks[i].c, checks[i].s, checks[i].a, checks[i].r, checks[i].args)) ok = false;
	}
	Method init = class_getInstanceMethod(objc_getClass("SBUISystemApertureContentProvider"), @selector(init));
	return ok && signature(init, "@", "");
}

static id keep(id obj)
{
	if (obj) { [probe_objects addObject:obj]; [obj release]; }
	return obj;
}
static void remove_probe(void)
{
	if (!probe_assertion) return;
	Method invalidate = class_getInstanceMethod(object_getClass(probe_assertion), sel_registerName("invalidate"));
	if (!signature(invalidate, "v", "")) { ilog("probe remove FAILED reason=invalidate-contract"); return; }
	((void (*)(id, SEL))objc_msgSend)(probe_assertion, sel_registerName("invalidate"));
	[probe_assertion release]; probe_assertion = nil;
	[probe_objects release]; probe_objects = nil;
	ilog("probe removed assertion-invalidated=1");
}
static void try_probe(void)
{
	if (level != 3 || !launched || probe_assertion) return;
	if (!aperture_window || !load_count || !geometry_ok) {
		ilog("probe deferred reason=%s", !geometry_ok ? "canvas-mismatch" : !aperture_window ? "no-native-window" : "view-not-loaded");
		return;
	}
	if (!probe_contracts()) { ilog("probe FAILED reason=contract"); return; }
	id controller = object_value([UIApplication sharedApplication], "systemApertureControllerForMainDisplay");
	if (!controller || ![controller isKindOfClass:objc_getClass("SBSystemApertureController")]) {
		ilog("probe FAILED reason=controller class=%s", clsname(controller)); return;
	}
	probe_objects = [[NSMutableArray alloc] init];
	id ident = keep(((id (*)(id, SEL, id, id))objc_msgSend)([objc_getClass("_SBSystemApertureElementIdentification") alloc],
	    sel_registerName("initWithClientIdentifier:elementIdentifier:"), @"com.redwood.island.probe", @"probe"));
	UILabel *leading = keep([[UILabel alloc] initWithFrame:CGRectMake(0, 0, 26, 22)]);
	UILabel *trailing = keep([[UILabel alloc] initWithFrame:CGRectMake(0, 0, 26, 22)]);
	leading.text = @"RW"; trailing.text = @"16";
	for (UILabel *label in @[leading, trailing]) {
		label.font = [UIFont boldSystemFontOfSize:12];
		label.textAlignment = NSTextAlignmentCenter;
		label.textColor = [UIColor whiteColor];
		label.backgroundColor = [UIColor colorWithRed:0 green:.32 blue:.26 alpha:1];
		label.layer.cornerRadius = 5;
		label.clipsToBounds = YES;
		label.userInteractionEnabled = NO;
	}
	id left = keep(((id (*)(id, SEL, id))objc_msgSend)([objc_getClass("SBUISystemApertureCustomContentProvider") alloc], sel_registerName("initWithView:"), leading));
	id right = keep(((id (*)(id, SEL, id))objc_msgSend)([objc_getClass("SBUISystemApertureCustomContentProvider") alloc], sel_registerName("initWithView:"), trailing));
	id composite = keep([[objc_getClass("SBUISystemApertureContentProvider") alloc] init]);
	if (!ident || !left || !right || !composite) { ilog("probe FAILED reason=provider-allocation"); goto failed; }
	((void (*)(id, SEL, id))objc_msgSend)(composite, sel_registerName("setLeadingContentViewProvider:"), left);
	((void (*)(id, SEL, id))objc_msgSend)(composite, sel_registerName("setTrailingContentViewProvider:"), right);
	id element = keep(((id (*)(id, SEL, id, id))objc_msgSend)([objc_getClass("SBSystemApertureProvidedContentElement") alloc],
	    sel_registerName("initWithIdentifier:contentProvider:"), ident, composite));
	if (!element) { ilog("probe FAILED reason=element-allocation"); goto failed; }
	const char *modes[] = {"setMinimumSupportedLayoutMode:", "setMaximumSupportedLayoutMode:", "setPreferredLayoutMode:"};
	for (unsigned i = 0; i < 3; i++) ((void (*)(id, SEL, NSInteger))objc_msgSend)(element, sel_registerName(modes[i]), 3);
	probe_assertion = [((id (*)(id, SEL, id))objc_msgSend)(controller, sel_registerName("registerElement:"), element) retain];
	ilog("probe registered assertion=%p mode=compact(3/3/3) retained=%lu", probe_assertion, (unsigned long)probe_objects.count);
	if (probe_assertion) return;
	ilog("probe FAILED reason=null-registration-assertion");
failed:
	[probe_objects release]; probe_objects = nil;
}

static void toggle_marker(void)
{
	if (level != 3 || !aperture_vc.isViewLoaded) { ilog("marker unavailable level=%d viewDidLoad=%u", level, load_count); return; }
	if (diagnostic_view) { [diagnostic_view removeFromSuperview]; [diagnostic_view release]; diagnostic_view = nil; ilog("marker removed"); return; }
	diagnostic_view = [[UIView alloc] initWithFrame:footprint];
	diagnostic_view.backgroundColor = [UIColor magentaColor];
	diagnostic_view.userInteractionEnabled = NO;
	diagnostic_view.layer.cornerRadius = footprint.size.height / 2;
	[aperture_vc.view addSubview:diagnostic_view];
	ilog("marker added native-scene=1 rect={125,4,125,36.667} diagnostic-only=1");
}

static void update_backing(UIView *view)
{
	UIView *backing = objc_getAssociatedObject(view, &backing_key);
	if (!ordinary_backing) {
		[backing removeFromSuperview];
		objc_setAssociatedObject(view, &backing_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		return;
	}
	if (!backing) {
		backing = [[UIView alloc] initWithFrame:view.bounds];
		backing.backgroundColor = [UIColor blackColor];
		backing.userInteractionEnabled = NO;
		backing.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
		[view insertSubview:backing atIndex:0];
		objc_setAssociatedObject(view, &backing_key, backing, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		[backing release];
		ilog("backing ordinary attached container=%p frame=%s", view, [NSStringFromCGRect(view.bounds) UTF8String]);
	}
	backing.frame = view.bounds;
	backing.layer.cornerRadius = MIN(view.bounds.size.height / 2, footprint.size.height / 2);
	backing.layer.cornerCurve = kCACornerCurveContinuous;
	SEL sel = sel_registerName("renderingConfiguration");
	Method m = class_getInstanceMethod(object_getClass(view), sel);
	if (signature(m, @encode(SBSystemApertureContainerRenderingConfiguration), "")) {
		SBSystemApertureContainerRenderingConfiguration config = ((SBSystemApertureContainerRenderingConfiguration (*)(id, SEL))objc_msgSend)(view, sel);
		backing.hidden = config.renderingStyle != 0;
	} else backing.hidden = YES;
}
static void update_backing_tree(UIView *view)
{
	if (!view) return;
	if ([view isKindOfClass:objc_getClass("SBSystemApertureContainerView")]) update_backing(view);
	for (UIView *child in view.subviews) update_backing_tree(child);
}
static void toggle_backing(void)
{
	if (level != 3 || !aperture_vc.isViewLoaded) { ilog("backing unavailable level=%d", level); return; }
	ordinary_backing = !ordinary_backing;
	update_backing_tree(aperture_vc.view);
	ilog("backing ordinary enabled=%d native-content-unchanged=1", ordinary_backing);
}

static NSString *short_text(id value)
{
	NSString *text = value ? [[value description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "] : @"nil";
	return text.length > 300 ? [text substringToIndex:300] : text;
}

static void trace_bool(Class cls, const char *name)
{
	SEL sel = sel_registerName(name);
	Method m = class_getInstanceMethod(cls, sel);
	if (!m || strcmp(method_getTypeEncoding(m), "B24@0:8@16")) { ilog("trace %s REJECTED", name); return; }
	BOOL (*original)(id, SEL, id) = (BOOL (*)(id, SEL, id))method_getImplementation(m);
	method_setImplementation(m, imp_implementationWithBlock(^BOOL(id self, id item) {
		BOOL result = original(self, sel, item);
		ilog("trace %s -> %d item=%s", name, result, short_text(item).UTF8String);
		return result;
	}));
	ilog("trace %s installed", name);
}

static void trace_void(Class cls, const char *name)
{
	SEL sel = sel_registerName(name);
	Method m = class_getInstanceMethod(cls, sel);
	if (!m || strcmp(method_getTypeEncoding(m), "v24@0:8@16")) { ilog("trace %s REJECTED", name); return; }
	void (*original)(id, SEL, id) = (void (*)(id, SEL, id))method_getImplementation(m);
	method_setImplementation(m, imp_implementationWithBlock(^(id self, id item) {
		ilog("trace %s item=%s", name, short_text(item).UTF8String);
		original(self, sel, item);
	}));
	ilog("trace %s installed", name);
}

static void trace_object(Class cls, const char *name)
{
	SEL sel = sel_registerName(name);
	Method m = class_getInstanceMethod(cls, sel);
	if (!m || strcmp(method_getTypeEncoding(m), "@24@0:8@16")) { ilog("trace %s REJECTED", name); return; }
	id (*original)(id, SEL, id) = (id (*)(id, SEL, id))method_getImplementation(m);
	method_setImplementation(m, imp_implementationWithBlock(^id(id self, id item) {
		id result = original(self, sel, item);
		ilog("trace %s -> %s", name, short_text(result).UTF8String);
		return result;
	}));
	ilog("trace %s installed", name);
}

static void trace_completion(Class cls, const char *name)
{
	SEL sel = sel_registerName(name);
	Method m = class_getInstanceMethod(cls, sel);
	if (!m || strcmp(method_getTypeEncoding(m), "v32@0:8@16@?24")) { ilog("trace %s REJECTED", name); return; }
	void (*original)(id, SEL, id, id) = (void (*)(id, SEL, id, id))method_getImplementation(m);
	method_setImplementation(m, imp_implementationWithBlock(^(id self, id item, id completion) {
		ilog("trace %s item=%s", name, short_text(item).UTF8String);
		original(self, sel, item, completion);
	}));
	ilog("trace %s installed", name);
}

static void activity_trace(void)
{
	Class cls = objc_getClass("SBActivitySystemApertureElementObserver");
	if (!cls) { ilog("trace observer MISSING"); return; }
	trace_void(cls, "activityDidStart:");
	trace_void(cls, "activityDidUpdate:");
	trace_void(cls, "activityDidEnd:");
	trace_void(cls, "activityWasBlockedForItem:");
	trace_bool(cls, "_shouldHandleActivityItem:");
	trace_bool(cls, "_canRegisterElementForActivityItem:");
	trace_void(cls, "_addPendingItemIfNecessary:");
	trace_object(cls, "_createSystemApertureSceneHandleWithItem:");
	trace_completion(cls, "_createAndActivateElementForActivityItem:completion:");
}

static void post_launch(void)
{
	if (launched) return;
	launched = true;
	suite_started = false;
	install_suite();
	activity_trace();
	ilog("launch completed main-thread=%d", [NSThread isMainThread]);
	int token;
	notify_register_dispatch("com.redwood.island.dump", &token, dispatch_get_main_queue(), ^(int t) { (void)t; diagnostics(); });
	notify_register_dispatch("com.redwood.island.probe", &token, dispatch_get_main_queue(), ^(int t) { (void)t; try_probe(); });
	notify_register_dispatch("com.redwood.island.remove", &token, dispatch_get_main_queue(), ^(int t) { (void)t; remove_probe(); });
	notify_register_dispatch("com.redwood.island.marker", &token, dispatch_get_main_queue(), ^(int t) { (void)t; toggle_marker(); });
	notify_register_dispatch("com.redwood.island.backing", &token, dispatch_get_main_queue(), ^(int t) { (void)t; toggle_backing(); });
	for (unsigned i = 1; i <= 5; i++) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, i * 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			@autoreleasepool {
				if (i == 1) diagnostics();
				try_probe();
				if (i == 5) diagnostics();
			}
		});
	}
}
static void (*original_launch)(id, SEL, id);
static void launch_hook(id self, SEL cmd, id notification)
{
	install_suite();
	ilog("launch entered");
	original_launch(self, cmd, notification);
	dispatch_async(dispatch_get_main_queue(), ^{ post_launch(); });
}

void redwood_island_init(void)
{
	char args[2048] = "";
	size_t n = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &n, NULL, 0)) return;
	for (char *p = args; *p;) {
		p += strspn(p, " \t\r\n");
		size_t len = strcspn(p, " \t\r\n");
		if (len >= 7 && !strncmp(p, "island=", 7)) level = len == 8 && p[7] >= '0' && p[7] <= '3' ? p[7] - '0' : 0;
		p += len;
	}
	if (!level) return;
	size_t length;
	const void *cache = _dyld_get_shared_cache_range(&length);
	if (!cache) { ilog("bootstrap FAILED reason=no-shared-cache"); return; }
	if (length < 104 || memcmp(cache, "dyld_v1   arm64", 16)) {
		ilog("bootstrap FAILED reason=cache-header"); return;
	}
	char cache_uuid[33];
	for (unsigned i = 0; i < 16; i++) snprintf(cache_uuid + i * 2, 3, "%02x", ((const unsigned char *)cache)[88 + i]);
	ilog("cache uuid=%s expected=e5420db330d033aa8b7e05b5038bb46d", cache_uuid);
	if (strcmp(cache_uuid, "e5420db330d033aa8b7e05b5038bb46d")) { ilog("bootstrap FAILED reason=cache-UUID"); return; }
	cache_slide = (uintptr_t)cache - 0x180000000;
	ilog("bootstrap process=SpringBoard library=libredwood-sb level=%d slide=%#lx", level, cache_slide);
	Method launch = class_getInstanceMethod(objc_getClass("SpringBoard"), sel_registerName("applicationDidFinishLaunching:"));
	if (!launch || !uuid_matches((void *)method_getImplementation(launch), "ea573065f56538ab9adf96f097ce20a5")) {
		ilog("bootstrap FAILED reason=SpringBoard-UUID"); return;
	}
	struct hook bootstrap[] = {
		H('-', "SpringBoard", "applicationDidFinishLaunching:", 0x1c4ab7ca0, "v", "@", launch_hook, original_launch),
		H('-', "SBEmbeddedDisplayWindowSceneDelegate", "_configureForConnectingWindowScene:windowSceneContext:", 0x1c4f65ecc, "v", "@@", configure_hook, original_configure),
		H('+', "UISDisplaySingleRectShape", "sb_thisDeviceDisplayShape", 0x1c4e7526c, "@", "", shape_hook, original_shape)
	};
	for (unsigned i = 0; i < sizeof(bootstrap) / sizeof(bootstrap[0]); i++) install(&bootstrap[i]);
	function_hooks();
	ilog("bootstrap armed installed=%u rejected=%u awaiting-natural-startup=1", installed, rejected);
}
