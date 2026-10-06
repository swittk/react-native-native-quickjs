package com.reactnativenativequickjs;

import androidx.annotation.NonNull;
import com.facebook.proguard.annotations.DoNotStrip;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.annotations.ReactModule;

@DoNotStrip
@ReactModule(name = QuickJSModule.NAME)
public final class QuickJSModule extends NativeQuickJSSpec {
  public static final String NAME = "SKNativeQuickJS";

  static {
    System.loadLibrary("SKRNNativeQuickJS");
  }

  private final ReactApplicationContext reactContext;

  public QuickJSModule(ReactApplicationContext reactContext) {
    super(reactContext);
    this.reactContext = reactContext;
  }

  @Override
  public boolean installBindings() {
    return QuickJSBindingInstaller.install(reactContext);
  }

  @NonNull
  @Override
  public String getName() {
    return NAME;
  }
}
