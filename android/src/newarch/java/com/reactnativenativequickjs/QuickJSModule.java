package com.reactnativenativequickjs;

import androidx.annotation.NonNull;
import com.facebook.proguard.annotations.DoNotStrip;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.annotations.ReactModule;
import com.facebook.react.turbomodule.core.interfaces.BindingsInstallerHolder;
import com.facebook.react.turbomodule.core.interfaces.TurboModuleWithJSIBindings;

@DoNotStrip
@ReactModule(name = QuickJSModule.NAME)
public final class QuickJSModule extends NativeQuickJSSpec implements TurboModuleWithJSIBindings {
  public static final String NAME = "SKNativeQuickJS";

  static {
    System.loadLibrary("SKRNNativeQuickJS");
  }

  public QuickJSModule(ReactApplicationContext reactContext) {
    super(reactContext);
  }

  @DoNotStrip
  @Override
  public native BindingsInstallerHolder getBindingsInstaller();

  @NonNull
  @Override
  public String getName() {
    return NAME;
  }

}
