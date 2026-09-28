#include "react-native-native-quickjs.h"

#include <jni.h>
#include <memory>

#include <ReactCommon/CallInvokerHolder.h>
#include <fbjni/fbjni.h>

extern "C" JNIEXPORT void JNICALL
Java_com_reactnativenativequickjs_QuickJSBindingInstaller_installNative(
    JNIEnv*,
    jclass,
    jlong runtimePointer,
    jobject callInvokerHolderObject) {
  auto holder = facebook::jni::alias_ref<
      facebook::react::CallInvokerHolder::javaobject>(
      reinterpret_cast<facebook::react::CallInvokerHolder::javaobject>(
          callInvokerHolderObject));
  SKRNNativeQuickJS::install(
      *reinterpret_cast<facebook::jsi::Runtime*>(runtimePointer),
      holder->cthis()->getCallInvoker());
}
