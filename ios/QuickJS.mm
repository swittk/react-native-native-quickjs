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

namespace {
class QuickJSTurboModule final : public react::NativeQuickJSSpecJSI {
 public:
  explicit QuickJSTurboModule(const react::ObjCTurboModule::InitParams &params)
      : react::NativeQuickJSSpecJSI(params)
  {
    methodMap_["installBindings"] =
        MethodMetadata{0, &QuickJSTurboModule::installBindings};
  }

 private:
  static jsi::Value installBindings(
      jsi::Runtime &runtime,
      react::TurboModule &turboModule,
      const jsi::Value *,
      size_t)
  {
    auto &module = static_cast<QuickJSTurboModule &>(turboModule);
    SKRNNativeQuickJS::install(runtime, module.jsInvoker_);
    return jsi::Value(true);
  }
};
} // namespace

- (std::shared_ptr<react::TurboModule>)getTurboModule:
    (const react::ObjCTurboModule::InitParams &)params
{
  return std::make_shared<QuickJSTurboModule>(params);
}

// Satisfies the generated NativeQuickJSSpec Objective-C protocol. New
// Architecture calls are intercepted by QuickJSTurboModule's method map above
// so the JSI runtime and CallInvoker are available at the install site.
- (NSNumber *)installBindings
{
  return @YES;
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
