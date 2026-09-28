package com.reactnativenativequickjs;

import androidx.annotation.NonNull;
import com.facebook.react.bridge.JavaScriptContextHolder;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
import com.facebook.react.bridge.ReactMethod;
import com.facebook.react.module.annotations.ReactModule;
import com.facebook.react.turbomodule.core.CallInvokerHolderImpl;

@ReactModule(name = QuickJSModule.NAME)
public final class QuickJSModule extends ReactContextBaseJavaModule {
  public static final String NAME = "SKNativeQuickJS";

  static {
    System.loadLibrary("SKRNNativeQuickJS");
  }

  private final ReactApplicationContext reactContext;

  public QuickJSModule(ReactApplicationContext reactContext) {
    super(reactContext);
    this.reactContext = reactContext;
  }

  private static native void installLegacy(
      long runtimePointer,
      CallInvokerHolderImpl callInvokerHolder);

  @NonNull
  @Override
  public String getName() {
    return NAME;
  }

  /**
   * Called synchronously from the JS thread only when the JSI factory is not
   * already installed. This avoids mutating Hermes from the native-modules
   * queue during module initialize/teardown.
   */
  @SuppressWarnings("deprecation")
  @ReactMethod(isBlockingSynchronousMethod = true)
  public boolean installBindings() {
    JavaScriptContextHolder jsContext = reactContext.getJavaScriptContextHolder();
    CallInvokerHolderImpl holder =
        (CallInvokerHolderImpl) reactContext
            .getCatalystInstance()
            .getJSCallInvokerHolder();
    if (jsContext.get() == 0 || holder == null) {
      return false;
    }
    installLegacy(jsContext.get(), holder);
    return true;
  }
}
