package com.reactnativenativequickjs;

import androidx.annotation.NonNull;
import com.facebook.react.bridge.JavaScriptContextHolder;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
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

  private static native void installLegacy(long runtimePointer, CallInvokerHolderImpl callInvokerHolder);
  private static native void cleanupLegacy(long runtimePointer);

  @NonNull
  @Override
  public String getName() {
    return NAME;
  }

  @SuppressWarnings("deprecation")
  @Override
  public void initialize() {
    super.initialize();
    JavaScriptContextHolder jsContext = reactContext.getJavaScriptContextHolder();
    CallInvokerHolderImpl holder =
        (CallInvokerHolderImpl) reactContext.getCatalystInstance().getJSCallInvokerHolder();
    if (jsContext.get() != 0 && holder != null) {
      installLegacy(jsContext.get(), holder);
    }
  }

  @SuppressWarnings("deprecation")
  @Override
  public void onCatalystInstanceDestroy() {
    JavaScriptContextHolder jsContext = reactContext.getJavaScriptContextHolder();
    if (jsContext.get() != 0) {
      cleanupLegacy(jsContext.get());
    }
    super.onCatalystInstanceDestroy();
  }
}
