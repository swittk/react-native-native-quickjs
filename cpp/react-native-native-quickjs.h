#ifndef REACT_NATIVE_NATIVE_QUICKJS_H
#define REACT_NATIVE_NATIVE_QUICKJS_H

#include <memory>

#include <jsi/jsi.h>

namespace facebook {
namespace react {
class CallInvoker;
}
}

namespace SKRNNativeQuickJS {

/** Installs the QuickJS runtime factory into the React Native JSI runtime. */
void install(
    facebook::jsi::Runtime& runtime,
    std::shared_ptr<facebook::react::CallInvoker> callInvoker);

/** Removes the factory during legacy bridge/runtime teardown. */
void cleanup(facebook::jsi::Runtime& runtime);

} // namespace SKRNNativeQuickJS

#endif
