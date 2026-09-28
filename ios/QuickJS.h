#import <Foundation/Foundation.h>

#ifdef RCT_NEW_ARCH_ENABLED
#import <NativeQuickJS/NativeQuickJS.h>
#import <ReactCommon/RCTTurboModuleWithJSIBindings.h>

@interface SKNativeQuickJS : NSObject <NativeQuickJSSpec, RCTTurboModuleWithJSIBindings>
#else
#import <React/RCTBridgeModule.h>

@interface SKNativeQuickJS : NSObject <RCTBridgeModule>
#endif

@end
