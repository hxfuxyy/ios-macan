#import <Foundation/Foundation.h>
#include <dlfcn.h>
@interface NSObject (Ace6TWorkspace)
+ (id)defaultWorkspace;
- (BOOL)openApplicationWithBundleID:(NSString *)bundle;
@end
int main(void) {
    @autoreleasepool {
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices",RTLD_NOW);
        Class c=NSClassFromString(@"LSApplicationWorkspace");
        if(!c || ![c respondsToSelector:@selector(defaultWorkspace)]) { NSLog(@"Sileo: LaunchServices unavailable");return 1; }
        id workspace=[c defaultWorkspace];
        if(![workspace respondsToSelector:@selector(openApplicationWithBundleID:)]) { NSLog(@"Sileo: launch API unavailable");return 1; }
        BOOL ok=[workspace openApplicationWithBundleID:@"org.coolstar.SileoStore"];
        NSLog(@"Sileo: launch request %@",ok?@"accepted":@"refused");
        return ok?0:1;
    }
}
