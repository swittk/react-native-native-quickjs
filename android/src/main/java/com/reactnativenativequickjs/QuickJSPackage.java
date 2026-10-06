package com.reactnativenativequickjs;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import com.facebook.react.TurboReactPackage;
import com.facebook.react.bridge.NativeModule;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.model.ReactModuleInfo;
import com.facebook.react.module.model.ReactModuleInfoProvider;
import com.facebook.react.uimanager.ViewManager;
import java.util.Collections;
import java.util.List;

public final class QuickJSPackage extends TurboReactPackage {
  @Nullable
  @Override
  public NativeModule getModule(@NonNull String name, @NonNull ReactApplicationContext reactContext) {
    return QuickJSModule.NAME.equals(name) ? new QuickJSModule(reactContext) : null;
  }

  @NonNull
  @Override
  public ReactModuleInfoProvider getReactModuleInfoProvider() {
    return () -> Collections.singletonMap(
        QuickJSModule.NAME,
        new ReactModuleInfo(
            QuickJSModule.NAME,
            QuickJSModule.class.getName(),
            false, false, false, false,
            BuildConfig.IS_NEW_ARCHITECTURE_ENABLED));
  }

  @NonNull
  @Override
  public List<NativeModule> createNativeModules(@NonNull ReactApplicationContext reactContext) {
    return Collections.singletonList(new QuickJSModule(reactContext));
  }

  @NonNull
  @Override
  public List<ViewManager> createViewManagers(@NonNull ReactApplicationContext reactContext) {
    return Collections.emptyList();
  }
}
