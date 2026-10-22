#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>

@interface PSSpecifier : NSObject
- (void)setProperty:(id)value forKey:(NSString *)key;
- (id)propertyForKey:(NSString *)key;
- (long long)cellType;
@end

@interface PSListController : UIViewController
- (NSArray *)loadSpecifiersFromPlistName:(NSString *)name target:(id)target;
@end

enum { PSGroupCell = 0 };

typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef allocator, CFStringRef name, void *callout, void *context);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef store, CFStringRef key);

#ifndef HACKIOS_BUILD
#define HACKIOS_BUILD "HackiOS (a build without its commit)"
#endif

static NSString *
sysctl_text(const char *name)
{
	char buffer[2048];
	size_t size = sizeof(buffer) - 1;

	if (sysctlbyname(name, buffer, &size, NULL, 0) != 0) {
		return nil;
	}
	buffer[size] = 0;
	return @(buffer);
}

static bool
sysctl_number(const char *name, long long *value)
{
	union {
		int32_t i32;
		int64_t i64;
	} u = { 0 };
	size_t size = sizeof(u);

	if (sysctlbyname(name, &u, &size, NULL, 0) != 0) {
		return false;
	}
	*value = size == sizeof(int32_t) ? u.i32 : u.i64;
	return true;
}

static NSString *
source_build(void)
{
	return @HACKIOS_BUILD;
}

static NSString *
source_kernel(void)
{
	NSString *version = sysctl_text("kern.version");
	NSRange colon = [version rangeOfString:@": "], semicolon = [version rangeOfString:@";"];

	if (version == nil || colon.location == NSNotFound || semicolon.location == NSNotFound || semicolon.location < colon.location) {
		return version ?: @"?";
	}
	return [NSString stringWithFormat:@"%@, built %@", sysctl_text("kern.osrelease") ?: @"?",
	           [version substringWithRange:NSMakeRange(colon.location + 2, semicolon.location - colon.location - 2)]];
}

static NSString *
source_bootargs(void)
{
	NSMutableArray *kept = [NSMutableArray array];

	for (NSString *word in [sysctl_text("kern.bootargs") componentsSeparatedByString:@" "]) {
		if (word.length != 0 && ![word hasPrefix:@"wifi_mac="]) {
			[kept addObject:word];
		}
	}
	return kept.count ? [kept componentsJoinedByString:@" "] : @"none";
}

static NSString *
source_uptime(void)
{
	struct timeval boot = { 0 }, now;
	size_t size = sizeof(boot);
	long seconds;

	if (sysctlbyname("kern.boottime", &boot, &size, NULL, 0) != 0) {
		return @"?";
	}
	gettimeofday(&now, NULL);
	seconds = now.tv_sec - boot.tv_sec;
	return seconds >= 3600 ? [NSString stringWithFormat:@"%ld h %ld min", seconds / 3600, seconds / 60 % 60] :
	       [NSString stringWithFormat:@"%ld min %ld s", seconds / 60, seconds % 60];
}

static NSString *
source_memory(void)
{
	vm_statistics64_data_t vm;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	long long total = 0;
	vm_size_t page = 0;

	sysctl_number("hw.memsize", &total);
	host_page_size(mach_host_self(), &page);
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) {
		return [NSString stringWithFormat:@"%.1f GB", total / 1073741824.0];
	}
	return [NSString stringWithFormat:@"%.1f GB, %llu MB free", total / 1073741824.0, (unsigned long long)vm.free_count * page >> 20];
}

static NSString *
source_processors(void)
{
	long long running = 0, khz = 0;

	sysctl_number("hw.activecpu", &running);
	sysctl_number("kern.redwood.cpu_khz", &khz);
	return khz > 0 ? [NSString stringWithFormat:@"%lld, up to %.2f GHz", running, khz / 1e6] :
	       [NSString stringWithFormat:@"%lld, the loader's speed", running];
}

static NSString *
source_gpu(void)
{
	long long khz = 0;

	return sysctl_number("kern.redwood.gpu_khz", &khz) && khz > 0 ? [NSString stringWithFormat:@"on, %lld MHz", khz / 1000] :
	       @"off (the screen is drawn by the processors)";
}

static NSString *
source_wifi(void)
{
	NSString *state = sysctl_text("kern.redwood.wifi") ?: @"none";
	SCDynamicStoreRef store;
	NSDictionary *airport = nil;
	long long rssi = 0;

	if (![state isEqualToString:@"joined"]) {
		return [state isEqualToString:@"none"] ? @"no radio (wifi=1 not given)" : [state isEqualToString:@"off"] ?
		       @"turned off" : [state isEqualToString:@"joining"] ? @"joining" : @"on, not joined";
	}
	sysctl_number("kern.redwood.wifi_rssi", &rssi);
	if ((store = SCDynamicStoreCreate(NULL, CFSTR("HackiOS"), NULL, NULL)) != NULL) {
		airport = CFBridgingRelease(SCDynamicStoreCopyValue(store, CFSTR("State:/Network/Interface/en0/AirPort")));
		CFRelease(store);
	}
	if ([airport isKindOfClass:NSDictionary.class] && [airport[@"SSID_STR"] isKindOfClass:NSString.class]) {
		return [NSString stringWithFormat:@"%@, %lld dBm", airport[@"SSID_STR"], rssi];
	}
	return [NSString stringWithFormat:@"joined, %lld dBm", rssi];
}

static NSString *
source_battery(void)
{
	long long present = 0, soc = 0, mv = 0, ma = 0;

	if (![[sysctl_text("kern.bootargs") componentsSeparatedByString:@" "] containsObject:@"battery=1"]) {
		return @"not read (battery=1 not given)";
	}
	if (!sysctl_number("kern.redwood.battery_present", &present) || !present) {
		return @"the fuel gauge sees no battery";
	}
	sysctl_number("kern.redwood.battery_soc", &soc);
	sysctl_number("kern.redwood.battery_voltage_mv", &mv);
	sysctl_number("kern.redwood.battery_current_ma", &ma);
	return [NSString stringWithFormat:@"%lld %%, %.2f V, %@", soc, mv / 1000.0, ma < 0 ?
	           [NSString stringWithFormat:@"charging at %lld mA", -ma] : [NSString stringWithFormat:@"%lld mA", ma]];
}

static NSString *
source_buttons(void)
{
	long long on = 0;

	return sysctl_number("kern.redwood.buttons", &on) && on ? @"running" : @"off";
}

static const struct {
	NSString *name;
	NSString *(*text)(void);
} sources[] = {
	{ @"build", source_build }, { @"kernel", source_kernel }, { @"bootargs", source_bootargs }, { @"uptime", source_uptime },
	{ @"memory", source_memory }, { @"processors", source_processors }, { @"gpu", source_gpu }, { @"wifi", source_wifi },
	{ @"battery", source_battery }, { @"buttons", source_buttons },
};

static NSString *
source_text(NSString *name)
{
	for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		if ([sources[i].name isEqualToString:name]) {
			return sources[i].text();
		}
	}
	return nil;
}

@interface HackiOSListController : PSListController
@end

@implementation HackiOSListController

- (NSArray *)specifiers
{
	NSMutableArray *list = [self valueForKey:@"_specifiers"];

	if (list == nil) {
		list = [[self loadSpecifiersFromPlistName:@"Root" target:self] mutableCopy];
		for (PSSpecifier *specifier in list) {
			NSString *source = [specifier propertyForKey:@"hackiosSource"];

			if (source != nil && [specifier cellType] == PSGroupCell) {
				[specifier setProperty:source_text(source) forKey:@"footerText"];
			}
		}
		[self setValue:list forKey:@"_specifiers"];
	}
	return list;
}

- (id)hackiosValue:(PSSpecifier *)specifier
{
	return source_text([specifier propertyForKey:@"hackiosSource"]);
}

- (void)hackiosLeave:(PSSpecifier *)specifier
{
	UIAlertController *ask = [UIAlertController alertControllerWithTitle:@"Return to the Loader?"
	    message:@"iOS stops at once and the phone goes back to the loader, as with five fingers held on the screen."
	    preferredStyle:UIAlertControllerStyleAlert];

	[ask addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[ask addAction:[UIAlertAction actionWithTitle:@"Return" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
		int one = 1;

		if (sysctlbyname("kern.redwood.leave", NULL, NULL, &one, sizeof(one)) != 0) {
			NSLog(@"HackiOS: kern.redwood.leave: %s", strerror(errno));
		}
	}]];
	[self presentViewController:ask animated:YES completion:nil];
}

@end
