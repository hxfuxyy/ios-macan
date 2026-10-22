#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <objc/runtime.h>

@interface PSSpecifier : NSObject
+ (instancetype)groupSpecifierWithName:(NSString *)name;
- (void)setProperty:(id)value forKey:(NSString *)key;
@property (nonatomic, retain) NSString *name;
@property (nonatomic, retain) NSString *identifier;
@end

@interface PSListController : UIViewController
@end

extern NSArray *SpecifiersFromPlist(NSDictionary *plist, PSSpecifier *previous, id target, NSString *plistName, NSBundle *bundle,
    NSString **title, NSString **specifierID, PSListController *callerList, NSMutableArray **bundleControllers);
extern NSString *PSLazilyLoadedBundleKey;

static NSString *const entry_directories[] = { @"/redwood/Library/PreferenceLoader/Preferences", @"/Library/PreferenceLoader/Preferences" };
static NSString *const bundle_directories[] = { @"/redwood/Library/PreferenceBundles", @"/Library/PreferenceBundles",
	                                        @"/System/Library/PreferenceBundles" };

static NSArray *
entry_specifiers(PSListController *root, NSDictionary *entry, NSString *title)
{
	NSString *name = entry[@"bundle"], *path = nil;
	NSMutableArray *controllers = nil;
	NSArray *specifiers;
	NSBundle *bundle;

	if (![name isKindOfClass:NSString.class]) {
		NSLog(@"HackiOS: the entry %@ names no bundle: left out", title);
		return nil;
	}
	for (unsigned i = 0; i < sizeof(bundle_directories) / sizeof(bundle_directories[0]) && path == nil; i++) {
		NSString *candidate = [NSString stringWithFormat:@"%@/%@.bundle", bundle_directories[i], name];

		path = [NSFileManager.defaultManager fileExistsAtPath:candidate] ? candidate : nil;
	}
	if (path == nil || (bundle = [NSBundle bundleWithPath:path]) == nil) {
		NSLog(@"HackiOS: the entry %@: no bundle %@ in any of the places for them: left out", title, name);
		return nil;
	}
	specifiers = SpecifiersFromPlist(@{ @"items": @[ entry ] }, nil, root, title, bundle, NULL, NULL, root, &controllers);
	if ([entry[@"isController"] boolValue]) {
		for (PSSpecifier *specifier in specifiers) {
			[specifier setProperty:path forKey:PSLazilyLoadedBundleKey];
			if (specifier.name == nil) {
				specifier.name = title;
			}
		}
	}
	return specifiers.count ? specifiers : nil;
}

static NSArray *(*root_specifiers)(id self, SEL _cmd);
static Ivar specifiers_ivar;

static NSArray *
our_root_specifiers(id self, SEL _cmd)
{
	bool first = object_getIvar(self, specifiers_ivar) == nil;
	NSArray *list = root_specifiers(self, _cmd);
	NSMutableArray *loaded = [NSMutableArray array], *mutable;
	NSUInteger at;

	if (!first || ![list isKindOfClass:NSMutableArray.class]) {
		return list;
	}
	for (unsigned d = 0; d < sizeof(entry_directories) / sizeof(entry_directories[0]); d++) {
		NSArray *files = [NSFileManager.defaultManager subpathsOfDirectoryAtPath:entry_directories[d] error:NULL];

		for (NSString *file in [files sortedArrayUsingSelector:@selector(compare:)]) {
			NSDictionary *plist = [file.pathExtension isEqualToString:@"plist"] ? [NSDictionary dictionaryWithContentsOfFile:
			    [entry_directories[d] stringByAppendingPathComponent:file]] : nil;
			NSArray *specifiers = [plist[@"entry"] isKindOfClass:NSDictionary.class] ? entry_specifiers(self, plist[@"entry"],
			    file.lastPathComponent.stringByDeletingPathExtension) : nil;

			if (specifiers != nil) {
				[loaded addObjectsFromArray:specifiers];
			}
		}
	}
	if (loaded.count == 0) {
		return list;
	}
	[loaded sortUsingComparator:^NSComparisonResult(PSSpecifier *a, PSSpecifier *b) {
		return [a.name localizedCaseInsensitiveCompare:b.name];
	}];
	[loaded insertObject:[PSSpecifier groupSpecifierWithName:nil] atIndex:0];
	mutable = (NSMutableArray *)list;
	at = mutable.count;
	for (NSUInteger i = 0; i < mutable.count; i++) {
		if ([[mutable[i] identifier] isEqualToString:@"TWITTER"]) {
			for (at = i + 1; at < mutable.count && ![[mutable[at] valueForKey:@"cellType"] isEqual:@0]; at++) {
			}
			break;
		}
	}
	[mutable insertObjects:loaded atIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(at, loaded.count)]];
	return list;
}

__attribute__((constructor))
static void
start(void)
{
	Class root = objc_getClass("PSUIPrefsListController"), list = objc_getClass("PSListController");
	Method method = root != Nil ? class_getInstanceMethod(root, @selector(specifiers)) : NULL;

	specifiers_ivar = list != Nil ? class_getInstanceVariable(list, "_specifiers") : NULL;
	if (method == NULL || specifiers_ivar == NULL) {
		NSLog(@"HackiOS: no PSUIPrefsListController -specifiers here: Settings' main list is left as it is");
		return;
	}
	root_specifiers = (NSArray *(*)(id, SEL))method_getImplementation(method);
	if (!class_addMethod(root, @selector(specifiers), (IMP)our_root_specifiers, method_getTypeEncoding(method))) {
		method_setImplementation(method, (IMP)our_root_specifiers);
	}
}
