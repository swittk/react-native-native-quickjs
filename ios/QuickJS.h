#import <Foundation/Foundation.h>

#ifdef RCT_NEW_ARCH_ENABLED
#import <NativeQuickJS/NativeQuickJS.h>

@interface SKNativeQuickJS : NSObject <NativeQuickJSSpec>
#else
#import <React/RCTBridgeModule.h>

@interface SKNativeQuickJS : NSObject <RCTBridgeModule>
#endif

@end
