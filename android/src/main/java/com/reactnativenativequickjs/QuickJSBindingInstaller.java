package com.reactnativenativequickjs;

import com.facebook.react.bridge.JavaScriptContextHolder;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.turbomodule.core.CallInvokerHolderImpl;
import java.lang.reflect.Method;

final class QuickJSBindingInstaller {
  private QuickJSBindingInstaller() {}

  private static native void installNative(
      long runtimePointer,
      CallInvokerHolderImpl callInvokerHolder);

  static boolean install(ReactApplicationContext reactContext) {
    JavaScriptContextHolder jsContext = reactContext.getJavaScriptContextHolder();
    if (jsContext == null || jsContext.get() == 0) {
      return false;
    }

    CallInvokerHolderImpl holder = findCallInvokerHolder(reactContext);
    if (holder == null) {
      return false;
    }

    installNative(jsContext.get(), holder);
    return true;
  }

  private static CallInvokerHolderImpl findCallInvokerHolder(
      ReactApplicationContext reactContext) {
    // RN 0.75+ exposes this directly on ReactContext (including bridgeless).
    // Reflection keeps this source compilable against RN 0.73.6, where the
    // direct method does not exist yet.
    try {
      Method method =
          reactContext.getClass().getMethod("getJSCallInvokerHolder");
      Object holder = method.invoke(reactContext);
      if (holder instanceof CallInvokerHolderImpl) {
        return (CallInvokerHolderImpl) holder;
      }
    } catch (ReflectiveOperationException | RuntimeException ignored) {
      // Fall through to the bridge-era API used by RN 0.73.x.
    }

    try {
      Object holder =
          reactContext.getCatalystInstance().getJSCallInvokerHolder();
      return holder instanceof CallInvokerHolderImpl
          ? (CallInvokerHolderImpl) holder
          : null;
    } catch (RuntimeException ignored) {
      return null;
    }
  }
}
