package com.reactnativenativequickjs;

import androidx.annotation.NonNull;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
import com.facebook.react.bridge.ReactMethod;
import com.facebook.react.module.annotations.ReactModule;

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

  @NonNull
  @Override
  public String getName() {
    return NAME;
  }

  /**
   * Called synchronously from the React Native JS thread only when the JSI
   * factories are not already installed.
   */
  @ReactMethod(isBlockingSynchronousMethod = true)
  public boolean installBindings() {
    return QuickJSBindingInstaller.install(reactContext);
  }
}
