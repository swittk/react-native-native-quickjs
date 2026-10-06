require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))
cxx_standard = ENV["RCT_NEW_ARCH_ENABLED"] == "1" ? "c++20" : "c++17"

Pod::Spec.new do |s|
  s.name         = "react-native-native-quickjs"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => "13.4" }
  s.source       = {
    :git => "https://github.com/swittk/react-native-native-quickjs.git",
    :tag => "#{s.version}"
  }

  s.source_files = [
    "ios/**/*.{h,m,mm}",
    "cpp/QuickJSRuntime.h",
    "cpp/QuickJSRuntime.cpp",
    "cpp/react-native-native-quickjs.h",
    "cpp/react-native-native-quickjs.cpp",
    "vendor/quickjs/quickjs.h",
    "vendor/quickjs/quickjs-atom.h",
    "vendor/quickjs/quickjs-opcode.h",
    "vendor/quickjs/dtoa.h",
    "vendor/quickjs/libregexp.h",
    "vendor/quickjs/libregexp-opcode.h",
    "vendor/quickjs/libunicode.h",
    "vendor/quickjs/libunicode-table.h",
    "vendor/quickjs/cutils.h",
    "vendor/quickjs/list.h",
    "vendor/quickjs/quickjs.c",
    "vendor/quickjs/dtoa.c",
    "vendor/quickjs/libregexp.c",
    "vendor/quickjs/libunicode.c",
    "vendor/quickjs/cutils.c"
  ]
  s.preserve_paths = [
    "vendor/QUICKJS_LICENSE",
    "vendor/QUICKJS_SHA256",
    "vendor/README.md"
  ]
  s.requires_arc = true
  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => cxx_standard,
    "GCC_C_LANGUAGE_STANDARD" => "gnu11",
    "GCC_PREPROCESSOR_DEFINITIONS" => '$(inherited) _GNU_SOURCE CONFIG_VERSION=\"2026-06-04\"',
    "OTHER_CFLAGS" => "$(inherited) -fwrapv",
    "HEADER_SEARCH_PATHS" => '$(inherited) "$(PODS_TARGET_SRCROOT)/cpp"'
  }

  install_modules_dependencies(s)
end
