#include "react-native-native-quickjs.h"

#include <ReactCommon/BindingsInstallerHolder.h>
#include <fbjni/fbjni.h>

namespace facebook::react {

class QuickJSModuleJSIBindings final
    : public jni::JavaClass<QuickJSModuleJSIBindings> {
 public:
  static constexpr const char* kJavaDescriptor =
      "Lcom/reactnativenativequickjs/QuickJSModule;";

  static void registerNatives() {
    javaClassLocal()->registerNatives({
        makeNativeMethod("getBindingsInstaller", getBindingsInstaller),
    });
  }

 private:
  static jni::local_ref<BindingsInstallerHolder::javaobject>
  getBindingsInstaller(jni::alias_ref<QuickJSModuleJSIBindings>) {
    return BindingsInstallerHolder::newObjectCxxArgs(
        [](jsi::Runtime& runtime, const std::shared_ptr<CallInvoker>& callInvoker) {
          SKRNNativeQuickJS::install(runtime, callInvoker);
        });
  }
};

} // namespace facebook::react

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  return facebook::jni::initialize(vm, [] {
    facebook::react::QuickJSModuleJSIBindings::registerNatives();
  });
}
