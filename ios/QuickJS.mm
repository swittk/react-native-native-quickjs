#import "QuickJS.h"

#import "react-native-native-quickjs.h"

#ifdef RCT_NEW_ARCH_ENABLED
#import <ReactCommon/RCTTurboModule.h>
#else
#import <React/RCTBridge+Private.h>

@interface RCTBridge (QuickJSCallInvoker)
- (std::shared_ptr<facebook::react::CallInvoker>)jsCallInvoker;
@end
#endif

using namespace facebook;

@implementation SKNativeQuickJS

RCT_EXPORT_MODULE()

+ (BOOL)requiresMainQueueSetup
{
  return NO;
}

#ifdef RCT_NEW_ARCH_ENABLED

- (std::shared_ptr<react::TurboModule>)getTurboModule:
    (const react::ObjCTurboModule::InitParams &)params
{
  return std::make_shared<react::NativeQuickJSSpecJSI>(params);
}

- (void)installJSIBindingsWithRuntime:(jsi::Runtime &)runtime
                          callInvoker:(const std::shared_ptr<react::CallInvoker> &)callInvoker
{
  SKRNNativeQuickJS::install(runtime, callInvoker);
}

#else

@synthesize bridge = _bridge;

/**
 * Legacy RN invokes blocking synchronous methods on the JS thread. Install JSI
 * bindings here on demand instead of mutating Hermes from setBridge: or module
 * teardown queues.
 */
RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(installBindings)
{
  RCTCxxBridge *cxxBridge = (RCTCxxBridge *)self.bridge;
  if (cxxBridge == nil || cxxBridge.runtime == nullptr) {
    return @NO;
  }

  auto *runtime = reinterpret_cast<jsi::Runtime *>(cxxBridge.runtime);
  SKRNNativeQuickJS::install(*runtime, [cxxBridge jsCallInvoker]);
  return @YES;
}

#endif

@end
