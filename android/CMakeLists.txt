cmake_minimum_required(VERSION 3.13)
project(SKRNNativeQuickJS)

set(CMAKE_C_STANDARD 11)
if(REACT_NATIVE_MINOR_VERSION GREATER_EQUAL 80)
  set(CMAKE_CXX_STANDARD 20)
else()
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QUICKJS_SOURCES
    ../vendor/quickjs/quickjs.c
    ../vendor/quickjs/dtoa.c
    ../vendor/quickjs/libregexp.c
    ../vendor/quickjs/libunicode.c
    ../vendor/quickjs/cutils.c)

set(RNQJS_SOURCES
    ../cpp/QuickJSRuntime.cpp
    ../cpp/react-native-native-quickjs.cpp
    cpp-adapter.cpp
    ${QUICKJS_SOURCES})

add_library(SKRNNativeQuickJS SHARED ${RNQJS_SOURCES})

target_compile_definitions(SKRNNativeQuickJS PRIVATE
    _GNU_SOURCE
    CONFIG_VERSION=\"2026-06-04\")

target_compile_options(SKRNNativeQuickJS PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-O2;-fwrapv>
    $<$<COMPILE_LANGUAGE:CXX>:-O2;-fexceptions;-frtti;-Wall>)

target_include_directories(SKRNNativeQuickJS PRIVATE
    ../cpp
    "${NODE_MODULES_DIR}/react-native/ReactCommon"
    "${NODE_MODULES_DIR}/react-native/ReactCommon/jsi"
    "${NODE_MODULES_DIR}/react-native/ReactCommon/callinvoker"
    "${NODE_MODULES_DIR}/react-native/ReactCommon/react/nativemodule/core"
    "${NODE_MODULES_DIR}/react-native/ReactAndroid/src/main/jni"
    "${NODE_MODULES_DIR}/react-native/ReactAndroid/src/main/java/com/facebook/react/turbomodule/core/jni"
    "${NODE_MODULES_DIR}/react-native/ReactAndroid/src/main/jni/react/turbomodule")

find_package(fbjni REQUIRED CONFIG)
find_package(ReactAndroid REQUIRED CONFIG)
if(REACT_NATIVE_MINOR_VERSION GREATER_EQUAL 76)
  target_link_libraries(SKRNNativeQuickJS
      ReactAndroid::reactnative
      ReactAndroid::jsi
      fbjni::fbjni
      android
      log
      m)
else()
  target_link_libraries(SKRNNativeQuickJS
      ReactAndroid::reactnativejni
      ReactAndroid::jsi
      fbjni::fbjni
      android
      log
      m)
endif()
