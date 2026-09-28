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
  return YES;
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

- (void)setBridge:(RCTBridge *)bridge
{
  _bridge = bridge;
  [self installLegacyBindingsWhenReady];
}

- (void)installLegacyBindingsWhenReady
{
  RCTCxxBridge *cxxBridge = (RCTCxxBridge *)self.bridge;
  if (cxxBridge.runtime == nullptr) {
    __weak SKNativeQuickJS *weakSelf = self;
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(NSEC_PER_MSEC)),
        dispatch_get_main_queue(), ^{
          [weakSelf installLegacyBindingsWhenReady];
        });
    return;
  }

  auto *runtime = reinterpret_cast<jsi::Runtime *>(cxxBridge.runtime);
  SKRNNativeQuickJS::install(*runtime, [cxxBridge jsCallInvoker]);
}

- (void)invalidate
{
  RCTCxxBridge *cxxBridge = (RCTCxxBridge *)self.bridge;
  if (cxxBridge.runtime != nullptr) {
    auto *runtime = reinterpret_cast<jsi::Runtime *>(cxxBridge.runtime);
    SKRNNativeQuickJS::cleanup(*runtime);
  }
}

#endif

@end
