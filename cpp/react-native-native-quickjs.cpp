#include "react-native-native-quickjs.h"
#include "QuickJSRuntime.h"

#include <ReactCommon/CallInvoker.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace SKRNNativeQuickJS {
namespace {

namespace jsi = facebook::jsi;
using facebook::react::CallInvoker;

constexpr int kMaxBridgeDepth = 32;
constexpr std::size_t kMaxBridgeNodes = 16'384;
constexpr double kMaxSafeInteger = 9'007'199'254'740'991.0;
constexpr double kMaxSizeValue = std::min(
    kMaxSafeInteger,
    static_cast<double>(std::numeric_limits<std::size_t>::max()));
constexpr double kMaxExecutionLimitMs = 300'000.0;
constexpr std::size_t kMaxWorkerOutstandingTasks = 4'096;
constexpr const char* kFactoryName = "SKRNNativeQuickJSCreateRuntime";
constexpr const char* kWorkerFactoryName = "SKRNNativeQuickJSCreateWorker";

void countNode(int depth, std::size_t& nodes) {
  if (depth > kMaxBridgeDepth || ++nodes > kMaxBridgeNodes) {
    throw std::runtime_error("Value exceeds QuickJS bridge conversion limits");
  }
}

struct JSIBridgeHelpers {
  std::optional<jsi::Function> getPrototypeOf;
  std::optional<jsi::Object> objectPrototype;
  std::optional<jsi::Function> keys;

  void ensure(jsi::Runtime& runtime) {
    if (keys.has_value()) {
      return;
    }
    auto objectConstructor =
        runtime.global().getPropertyAsObject(runtime, "Object");
    getPrototypeOf.emplace(
        objectConstructor.getPropertyAsFunction(runtime, "getPrototypeOf"));
    objectPrototype.emplace(
        objectConstructor.getPropertyAsObject(runtime, "prototype"));
    keys.emplace(
        objectConstructor.getPropertyAsFunction(runtime, "keys"));
  }
};

bool isPlainObject(
    jsi::Runtime& runtime,
    const jsi::Object& object,
    JSIBridgeHelpers& helpers) {
  if (object.isHostObject(runtime) || object.isFunction(runtime) ||
      object.isArrayBuffer(runtime)) {
    return false;
  }

  helpers.ensure(runtime);
  const auto actual = helpers.getPrototypeOf->call(runtime, object);
  return actual.isNull() ||
      (actual.isObject() && jsi::Object::strictEquals(
          runtime, actual.asObject(runtime), *helpers.objectPrototype));
}

bool isPlainObject(
    jsi::Runtime& runtime,
    const jsi::Object& object) {
  JSIBridgeHelpers helpers;
  return isPlainObject(runtime, object, helpers);
}

rnquickjs::Value fromJSI(
    jsi::Runtime& runtime,
    const jsi::Value& value,
    int depth,
    std::size_t& nodes,
    JSIBridgeHelpers& helpers) {
  countNode(depth, nodes);
  if (value.isUndefined()) {
    return rnquickjs::Value{};
  }
  if (value.isNull()) {
    return rnquickjs::Value{nullptr};
  }
  if (value.isBool()) {
    return rnquickjs::Value{value.getBool()};
  }
  if (value.isNumber()) {
    return rnquickjs::Value{value.asNumber()};
  }
  if (value.isString()) {
    return rnquickjs::Value{value.asString(runtime).utf8(runtime)};
  }
  if (!value.isObject()) {
    throw std::runtime_error(
        "QuickJS bridge values support only undefined, null, booleans, numbers, strings, arrays, and plain objects");
  }

  auto object = value.asObject(runtime);
  if (object.isArray(runtime)) {
    auto array = object.asArray(runtime);
    const auto size = array.size(runtime);
    if (size > kMaxBridgeNodes) {
      throw std::runtime_error("Array exceeds QuickJS bridge conversion limits");
    }
    rnquickjs::Value::Array result;
    result.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
      const auto item = array.getValueAtIndex(runtime, index);
      result.push_back(
          fromJSI(runtime, item, depth + 1, nodes, helpers));
    }
    return rnquickjs::Value{std::move(result)};
  }

  if (!isPlainObject(runtime, object, helpers)) {
    throw std::runtime_error("Only plain objects can cross the QuickJS bridge");
  }

  helpers.ensure(runtime);
  auto keys =
      helpers.keys->call(runtime, object).asObject(runtime).asArray(runtime);
  const auto size = keys.size(runtime);
  if (size > kMaxBridgeNodes) {
    throw std::runtime_error("Object exceeds QuickJS bridge conversion limits");
  }

  rnquickjs::Value::Object result;
  for (std::size_t index = 0; index < size; ++index) {
    const auto keyValue = keys.getValueAtIndex(runtime, index);
    const auto keyString = keyValue.asString(runtime);
    const auto key = keyString.utf8(runtime);
    const auto item = object.getProperty(runtime, keyString);
    result.emplace(
        key,
        fromJSI(runtime, item, depth + 1, nodes, helpers));
  }
  return rnquickjs::Value{std::move(result)};
}

rnquickjs::Value fromJSI(jsi::Runtime& runtime, const jsi::Value& value) {
  std::size_t nodes = 0;
  JSIBridgeHelpers helpers;
  return fromJSI(runtime, value, 0, nodes, helpers);
}

void defineOwnDataProperty(
    jsi::Runtime& runtime,
    jsi::Function& defineProperty,
    jsi::Object& descriptor,
    jsi::Object& target,
    jsi::Value key,
    jsi::Value value) {
  descriptor.setProperty(runtime, "value", std::move(value));
  defineProperty.call(
      runtime,
      target,
      std::move(key),
      descriptor);
}

jsi::Value toJSI(
    jsi::Runtime& runtime,
    const rnquickjs::Value& value,
    int depth,
    std::size_t& nodes,
    jsi::Function* defineProperty,
    jsi::Object* descriptor) {
  countNode(depth, nodes);
  if (std::holds_alternative<std::monostate>(value.data)) {
    return jsi::Value::undefined();
  }
  if (std::holds_alternative<std::nullptr_t>(value.data)) {
    return jsi::Value::null();
  }
  if (const auto* boolean = std::get_if<bool>(&value.data)) {
    return jsi::Value(*boolean);
  }
  if (const auto* number = std::get_if<double>(&value.data)) {
    return jsi::Value(*number);
  }
  if (const auto* string = std::get_if<std::string>(&value.data)) {
    return jsi::String::createFromUtf8(runtime, *string);
  }
  std::optional<jsi::Function> ownedDefineProperty;
  std::optional<jsi::Object> ownedDescriptor;
  if (defineProperty == nullptr) {
    ownedDefineProperty.emplace(
        runtime.global()
            .getPropertyAsObject(runtime, "Object")
            .getPropertyAsFunction(runtime, "defineProperty"));
    ownedDescriptor.emplace(runtime);
    ownedDescriptor->setProperty(runtime, "writable", true);
    ownedDescriptor->setProperty(runtime, "enumerable", true);
    ownedDescriptor->setProperty(runtime, "configurable", true);
    defineProperty = &*ownedDefineProperty;
    descriptor = &*ownedDescriptor;
  }

  if (const auto* values = std::get_if<rnquickjs::Value::Array>(&value.data)) {
    jsi::Array result(runtime, values->size());
    for (std::size_t index = 0; index < values->size(); ++index) {
      defineOwnDataProperty(
          runtime,
          *defineProperty,
          *descriptor,
          result,
          jsi::Value(static_cast<double>(index)),
          toJSI(
              runtime,
              (*values)[index],
              depth + 1,
              nodes,
              defineProperty,
              descriptor));
    }
    return result;
  }

  jsi::Object result(runtime);
  for (const auto& [key, item] : std::get<rnquickjs::Value::Object>(value.data)) {
    defineOwnDataProperty(
        runtime,
        *defineProperty,
        *descriptor,
        result,
        jsi::String::createFromUtf8(runtime, key),
        toJSI(
            runtime,
            item,
            depth + 1,
            nodes,
            defineProperty,
            descriptor));
  }
  return result;
}

jsi::Value toJSI(jsi::Runtime& runtime, const rnquickjs::Value& value) {
  std::size_t nodes = 0;
  return toJSI(runtime, value, 0, nodes, nullptr, nullptr);
}

jsi::Object memoryToJSI(
    jsi::Runtime& runtime,
    const rnquickjs::MemoryStats& memory) {
  jsi::Object result(runtime);
  result.setProperty(runtime, "mallocBytes", static_cast<double>(memory.mallocBytes));
  result.setProperty(
      runtime, "memoryUsedBytes", static_cast<double>(memory.memoryUsedBytes));
  result.setProperty(runtime, "mallocCount", static_cast<double>(memory.mallocCount));
  result.setProperty(runtime, "objectCount", static_cast<double>(memory.objectCount));
  result.setProperty(runtime, "atomCount", static_cast<double>(memory.atomCount));
  return result;
}

jsi::Object resultToJSI(
    jsi::Runtime& runtime,
    const rnquickjs::ExecutionResult& result) {
  jsi::Object value(runtime);
  value.setProperty(
      runtime, "reason", jsi::String::createFromUtf8(runtime, result.reason));
  value.setProperty(runtime, "code", result.code);
  value.setProperty(
      runtime,
      "value",
      result.value.has_value() ? toJSI(runtime, *result.value)
                               : jsi::Value::undefined());

  jsi::Object error(runtime);
  error.setProperty(
      runtime, "name", jsi::String::createFromUtf8(runtime, result.error.name));
  error.setProperty(
      runtime,
      "message",
      jsi::String::createFromUtf8(runtime, result.error.message));
  error.setProperty(
      runtime, "stack", jsi::String::createFromUtf8(runtime, result.error.stack));
  value.setProperty(runtime, "error", std::move(error));
  value.setProperty(runtime, "durationMs", result.durationMs);
  value.setProperty(runtime, "memory", memoryToJSI(runtime, result.memory));
  value.setProperty(runtime, "outputTruncated", result.outputTruncated);
  return value;
}

bool optionalBoolean(
    jsi::Runtime& runtime,
    const jsi::Object& object,
    const char* name,
    bool fallback) {
  const auto value = object.getProperty(runtime, name);
  if (value.isUndefined()) {
    return fallback;
  }
  if (!value.isBool()) {
    throw jsi::JSError(runtime, std::string(name) + " must be a boolean");
  }
  return value.getBool();
}

double optionalNumber(
    jsi::Runtime& runtime,
    const jsi::Object& object,
    const char* name,
    double fallback) {
  const auto value = object.getProperty(runtime, name);
  if (value.isUndefined()) {
    return fallback;
  }
  if (!value.isNumber() || !std::isfinite(value.asNumber())) {
    throw jsi::JSError(runtime, std::string(name) + " must be a finite number");
  }
  return value.asNumber();
}

std::size_t optionalSize(
    jsi::Runtime& runtime,
    const jsi::Object& object,
    const char* name,
    std::size_t fallback) {
  const double value = optionalNumber(runtime, object, name, fallback);
  if (value < 0 || value > kMaxSizeValue) {
    throw jsi::JSError(runtime, std::string(name) + " is outside the supported range");
  }
  return static_cast<std::size_t>(value);
}

rnquickjs::RuntimeOptions runtimeOptions(
    jsi::Runtime& runtime,
    const jsi::Value* arguments,
    std::size_t count) {
  rnquickjs::RuntimeOptions result;
  if (count == 0 || arguments[0].isUndefined()) {
    return result;
  }
  if (!arguments[0].isObject()) {
    throw jsi::JSError(runtime, "Runtime options must be an object");
  }
  const auto options = arguments[0].asObject(runtime);
  if (!isPlainObject(runtime, options)) {
    throw jsi::JSError(runtime, "Runtime options must be a plain object");
  }
  const double requestedExecutionLimit = optionalNumber(
      runtime, options, "executionLimitMs", result.executionLimitMs);
  result.executionLimitMs = requestedExecutionLimit <= 0
      ? 0
      : static_cast<std::int64_t>(
            std::ceil(
                std::min(requestedExecutionLimit, kMaxExecutionLimitMs)));
  result.memoryLimitBytes = optionalSize(
      runtime, options, "memoryLimitBytes", result.memoryLimitBytes);
  result.maxStackBytes =
      optionalSize(runtime, options, "maxStackBytes", result.maxStackBytes);
  result.maxOutputBytes =
      optionalSize(runtime, options, "maxOutputBytes", result.maxOutputBytes);
  result.maxOutputLines =
      optionalSize(runtime, options, "maxOutputLines", result.maxOutputLines);
  result.collectResultMemoryStats = optionalBoolean(
      runtime, options, "collectResultMemoryStats", result.collectResultMemoryStats);
  return result;
}

std::string requiredString(
    jsi::Runtime& runtime,
    const jsi::Value* arguments,
    std::size_t count,
    std::size_t index,
    const char* label) {
  if (index >= count || !arguments[index].isString()) {
    throw jsi::JSError(runtime, std::string(label) + " must be a string");
  }
  return arguments[index].asString(runtime).utf8(runtime);
}

void validateFilename(
    jsi::Runtime& runtime,
    const std::string& filename) {
  if (filename.find('\0') != std::string::npos) {
    throw jsi::JSError(
        runtime, "QuickJS filename cannot contain embedded NUL characters");
  }
}

std::string requiredModuleName(
    jsi::Runtime& runtime,
    const jsi::Value* arguments,
    std::size_t count,
    std::size_t index) {
  auto name = requiredString(runtime, arguments, count, index, "Module name");
  if (name.empty()) {
    throw jsi::JSError(runtime, "Module name cannot be empty");
  }
  if (name.find('\0') != std::string::npos) {
    throw jsi::JSError(
        runtime, "Module name cannot contain embedded NUL characters");
  }
  return name;
}

std::uint64_t requiredHandle(
    jsi::Runtime& runtime,
    const jsi::Value* arguments,
    std::size_t count,
    std::size_t index) {
  if (index >= count || !arguments[index].isNumber()) {
    throw jsi::JSError(runtime, "QuickJS handle must be a number");
  }
  const auto handle = arguments[index].asNumber();
  if (!std::isfinite(handle) || handle < 1 || handle > kMaxSafeInteger ||
      std::floor(handle) != handle) {
    throw jsi::JSError(runtime, "QuickJS handle must be a positive safe integer");
  }
  return static_cast<std::uint64_t>(handle);
}

rnquickjs::EvalMode evalMode(
    jsi::Runtime& runtime,
    const jsi::Object& options) {
  const auto value = options.getProperty(runtime, "mode");
  if (value.isUndefined()) {
    return rnquickjs::EvalMode::Script;
  }
  if (!value.isString()) {
    throw jsi::JSError(runtime, "evaluate mode must be a string");
  }
  const auto mode = value.asString(runtime).utf8(runtime);
  if (mode == "script") {
    return rnquickjs::EvalMode::Script;
  }
  if (mode == "module") {
    return rnquickjs::EvalMode::Module;
  }
  if (mode == "async-script") {
    return rnquickjs::EvalMode::AsyncScript;
  }
  if (mode == "async-module") {
    return rnquickjs::EvalMode::AsyncModule;
  }
  throw jsi::JSError(runtime, "Unknown QuickJS evaluate mode: " + mode);
}

jsi::Function makeFunction(
    jsi::Runtime& runtime,
    const char* name,
    unsigned int argumentCount,
    jsi::HostFunctionType function) {
  return jsi::Function::createFromHostFunction(
      runtime,
      jsi::PropNameID::forAscii(runtime, name),
      argumentCount,
      std::move(function));
}

rnquickjs::Value invokeJSCallback(
    jsi::Runtime& runtime,
    const jsi::Function& callback,
    const std::vector<rnquickjs::Value>& arguments) {
  std::vector<jsi::Value> values;
  values.reserve(arguments.size());
  for (const auto& argument : arguments) {
    values.push_back(toJSI(runtime, argument));
  }
  const jsi::Value* data = values.data();
  const auto result = callback.call(runtime, data, values.size());
  return fromJSI(runtime, result);
}

rnquickjs::Value invokeOnJSThread(
    jsi::Runtime& runtime,
    const std::shared_ptr<CallInvoker>& callInvoker,
    const std::thread::id& jsThread,
    const std::shared_ptr<jsi::Function>& callback,
    const std::vector<rnquickjs::Value>& arguments) {
  if (std::this_thread::get_id() == jsThread) {
    return invokeJSCallback(runtime, *callback, arguments);
  }
  if (!callInvoker) {
    throw std::runtime_error("React Native CallInvoker is unavailable");
  }

  struct Invocation {
    std::mutex mutex;
    std::condition_variable ready;
    bool complete = false;
    std::optional<rnquickjs::Value> result;
    std::exception_ptr error;
  };
  auto invocation = std::make_shared<Invocation>();
  callInvoker->invokeAsync([
      &runtime, callback, arguments, invocation]() {
    try {
      invocation->result = invokeJSCallback(runtime, *callback, arguments);
    } catch (...) {
      invocation->error = std::current_exception();
    }
    {
      std::lock_guard<std::mutex> lock(invocation->mutex);
      invocation->complete = true;
    }
    invocation->ready.notify_one();
  });

  std::unique_lock<std::mutex> lock(invocation->mutex);
  invocation->ready.wait(lock, [&invocation] { return invocation->complete; });
  if (invocation->error) {
    std::rethrow_exception(invocation->error);
  }
  return std::move(*invocation->result);
}

struct WorkerCallbackRegistry {
  std::vector<std::shared_ptr<jsi::Function>> callbacks;
  // One owner reference is held by WorkerHostObject until shutdown.
  // Each queued JS dispatch adds another reference.
  std::atomic<std::size_t> pendingDispatches{1};
  std::atomic<bool> shutdown{false};
};

void finishWorkerCallbackDispatch(WorkerCallbackRegistry* registry) noexcept {
  if (registry == nullptr) {
    return;
  }
  const auto previous =
      registry->pendingDispatches.fetch_sub(1, std::memory_order_acq_rel);
  if (previous == 1 &&
      registry->shutdown.load(std::memory_order_acquire)) {
    delete registry;
  }
}

void releaseWorkerCallback(
    WorkerCallbackRegistry* registry,
    jsi::Function* callback) noexcept {
  if (registry == nullptr || callback == nullptr) {
    return;
  }
  const auto found = std::find_if(
      registry->callbacks.begin(),
      registry->callbacks.end(),
      [callback](const std::shared_ptr<jsi::Function>& item) {
        return item.get() == callback;
      });
  if (found != registry->callbacks.end()) {
    registry->callbacks.erase(found);
  }
}

rnquickjs::ErrorInfo errorFromJSI(
    jsi::Runtime& runtime,
    const jsi::Value& value) {
  rnquickjs::ErrorInfo result;
  result.name = "Error";

  if (value.isString()) {
    result.message = value.asString(runtime).utf8(runtime);
    return result;
  }

  if (value.isObject()) {
    auto object = value.asObject(runtime);
    const auto name = object.getProperty(runtime, "name");
    const auto message = object.getProperty(runtime, "message");
    const auto stack = object.getProperty(runtime, "stack");
    if (name.isString()) {
      result.name = name.asString(runtime).utf8(runtime);
    }
    if (message.isString()) {
      result.message = message.asString(runtime).utf8(runtime);
    }
    if (stack.isString()) {
      result.stack = stack.asString(runtime).utf8(runtime);
    }
  }

  if (result.message.empty()) {
    try {
      const auto stringFunction =
          runtime.global().getPropertyAsFunction(runtime, "String");
      result.message =
          stringFunction.call(runtime, value).asString(runtime).utf8(runtime);
    } catch (...) {
      result.message = "Host Promise rejected";
    }
  }
  return result;
}

void invokeAsyncOnJSThread(
    jsi::Runtime& runtime,
    const std::shared_ptr<CallInvoker>& callInvoker,
    WorkerCallbackRegistry* registry,
    jsi::Function* callback,
    const std::vector<rnquickjs::Value>& arguments,
    rnquickjs::QuickJSContext::AsyncHostCompletion completion) {
  if (!callInvoker || registry == nullptr || callback == nullptr) {
    rnquickjs::QuickJSContext::AsyncHostResult result;
    result.ok = false;
    result.error.name = "HostError";
    result.error.message = "React Native CallInvoker is unavailable";
    completion(std::move(result));
    return;
  }

  if (registry->shutdown.load(std::memory_order_acquire)) {
    rnquickjs::QuickJSContext::AsyncHostResult result;
    result.ok = false;
    result.error.name = "HostError";
    result.error.message = "QuickJS worker is disposed";
    completion(std::move(result));
    return;
  }

  registry->pendingDispatches.fetch_add(1, std::memory_order_acq_rel);
  try {
    callInvoker->invokeAsync([
        &runtime,
        registry,
        callback,
        arguments,
        completion = std::move(completion)]() mutable {
      struct DispatchGuard {
        WorkerCallbackRegistry* registry;
        ~DispatchGuard() { finishWorkerCallbackDispatch(registry); }
      } guard{registry};

      if (registry->shutdown.load(std::memory_order_acquire)) {
        return;
      }

      auto settle = std::make_shared<std::atomic<bool>>(false);
    const auto completeOnce =
        [settle, completion](rnquickjs::QuickJSContext::AsyncHostResult result) {
          bool expected = false;
          if (settle->compare_exchange_strong(expected, true)) {
            completion(std::move(result));
          }
        };

    try {
      std::vector<jsi::Value> values;
      values.reserve(arguments.size());
      for (const auto& argument : arguments) {
        values.push_back(toJSI(runtime, argument));
      }
      const jsi::Value* data = values.data();
      jsi::Value result =
          callback->call(runtime, data, values.size());

      if (result.isObject()) {
        auto object = result.asObject(runtime);
        const auto thenValue = object.getProperty(runtime, "then");
        if (thenValue.isObject() &&
            thenValue.asObject(runtime).isFunction(runtime)) {
          auto resolve = makeFunction(
              runtime,
              "resolveQuickJSHostPromise",
              1,
              [completeOnce](
                  jsi::Runtime& rt,
                  const jsi::Value&,
                  const jsi::Value* args,
                  std::size_t count) mutable -> jsi::Value {
                rnquickjs::QuickJSContext::AsyncHostResult settled;
                try {
                  settled.value =
                      count == 0 ? rnquickjs::Value{} : fromJSI(rt, args[0]);
                } catch (const std::exception& error) {
                  settled.ok = false;
                  settled.error.name = "HostValueError";
                  settled.error.message = error.what();
                }
                completeOnce(std::move(settled));
                return jsi::Value::undefined();
              });
          auto reject = makeFunction(
              runtime,
              "rejectQuickJSHostPromise",
              1,
              [completeOnce](
                  jsi::Runtime& rt,
                  const jsi::Value&,
                  const jsi::Value* args,
                  std::size_t count) mutable -> jsi::Value {
                rnquickjs::QuickJSContext::AsyncHostResult settled;
                settled.ok = false;
                settled.error = count == 0
                    ? rnquickjs::ErrorInfo{"Error", "Host Promise rejected", ""}
                    : errorFromJSI(rt, args[0]);
                completeOnce(std::move(settled));
                return jsi::Value::undefined();
              });

          auto thenFunction = thenValue.asObject(runtime).asFunction(runtime);
          thenFunction.callWithThis(
              runtime,
              object,
              {std::move(resolve), std::move(reject)});
          return;
        }
      }

      rnquickjs::QuickJSContext::AsyncHostResult settled;
      settled.value = fromJSI(runtime, result);
      completeOnce(std::move(settled));
    } catch (const std::exception& error) {
      rnquickjs::QuickJSContext::AsyncHostResult settled;
      settled.ok = false;
      settled.error.name = "HostError";
      settled.error.message = error.what();
      completeOnce(std::move(settled));
      } catch (...) {
        rnquickjs::QuickJSContext::AsyncHostResult settled;
        settled.ok = false;
        settled.error.name = "HostError";
        settled.error.message = "Unknown host async callback failure";
        completeOnce(std::move(settled));
      }
    });
  } catch (...) {
    finishWorkerCallbackDispatch(registry);
    throw;
  }
}


struct WorkerTaskResult {
  std::uint64_t taskId = 0;
  rnquickjs::ExecutionResult result;
  std::string output;
  jsi::Function* callbackToReleaseOnFailure = nullptr;
};

class WorkerHostObject final : public jsi::HostObject,
                               public std::enable_shared_from_this<WorkerHostObject> {
 public:
  using Command =
      std::function<void(rnquickjs::QuickJSRuntime&, rnquickjs::QuickJSContext&)>;

  WorkerHostObject(
      jsi::Runtime& hostRuntime,
      std::shared_ptr<CallInvoker> callInvoker,
      rnquickjs::RuntimeOptions options)
      : hostRuntime_(hostRuntime),
        callInvoker_(std::move(callInvoker)),
        options_(options) {
    auto callbackRegistry = std::make_unique<WorkerCallbackRegistry>();
    worker_ = std::thread([this] { workerMain(); });
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return ready_; });
    if (!startupError_.empty()) {
      lock.unlock();
      if (worker_.joinable()) {
        worker_.join();
      }
      throw std::runtime_error(startupError_);
    }
    callbackRegistry_ = callbackRegistry.release();
  }

  ~WorkerHostObject() override {
    shutdown();
  }

  jsi::Value get(
      jsi::Runtime& runtime,
      const jsi::PropNameID& name) override {
    const auto property = name.utf8(runtime);
    const std::weak_ptr<WorkerHostObject> weakSelf = shared_from_this();

    if (property == "valid") {
      return jsi::Value(
          !disposed_.load(std::memory_order_relaxed) &&
          !failed_.load(std::memory_order_relaxed));
    }
    if (property == "executing") {
      return jsi::Value(executing_.load(std::memory_order_relaxed));
    }

    if (property == "startEvaluate") {
      return makeFunction(runtime, "startEvaluate", 2, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value* args,
          std::size_t count) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto source =
            requiredString(rt, args, count, 0, "Source");

        std::string filename = "<worker>";
        auto mode = rnquickjs::EvalMode::AsyncScript;
        if (count > 1 && !args[1].isUndefined()) {
          if (!args[1].isObject()) {
            throw jsi::JSError(rt, "Evaluate options must be an object");
          }
          const auto options = args[1].asObject(rt);
          if (!isPlainObject(rt, options)) {
            throw jsi::JSError(rt, "Evaluate options must be a plain object");
          }
          const auto filenameValue = options.getProperty(rt, "filename");
          if (!filenameValue.isUndefined()) {
            if (!filenameValue.isString()) {
              throw jsi::JSError(rt, "Evaluate filename must be a string");
            }
            filename = filenameValue.asString(rt).utf8(rt);
            validateFilename(rt, filename);
          }
          const auto modeValue = options.getProperty(rt, "mode");
          if (!modeValue.isUndefined()) {
            mode = evalMode(rt, options);
          }
        }

        const auto taskId = self->startTask([
            source,
            filename,
            mode](
                rnquickjs::QuickJSRuntime&,
                rnquickjs::QuickJSContext& context) {
          return context.evaluateAwaited(source, filename, mode);
        });
        return jsi::Value(static_cast<double>(taskId));
      });
    }

    if (property == "startRetain") {
      return makeFunction(runtime, "startRetain", 2, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value* args,
          std::size_t count) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto source =
            requiredString(rt, args, count, 0, "Retained source or global");
        bool global = false;
        std::string filename = "<retain>";
        if (count > 1 && !args[1].isUndefined()) {
          if (!args[1].isObject()) {
            throw jsi::JSError(rt, "Retain options must be an object");
          }
          const auto options = args[1].asObject(rt);
          if (!isPlainObject(rt, options)) {
            throw jsi::JSError(rt, "Retain options must be a plain object");
          }
          const auto globalValue = options.getProperty(rt, "global");
          if (!globalValue.isUndefined()) {
            if (!globalValue.isBool()) {
              throw jsi::JSError(rt, "Retain global must be a boolean");
            }
            global = globalValue.getBool();
          }
          const auto filenameValue = options.getProperty(rt, "filename");
          if (!filenameValue.isUndefined()) {
            if (!filenameValue.isString()) {
              throw jsi::JSError(rt, "Retain filename must be a string");
            }
            filename = filenameValue.asString(rt).utf8(rt);
            validateFilename(rt, filename);
          }
        }

        const auto taskId = self->startTask([
            source,
            filename,
            global](
                rnquickjs::QuickJSRuntime& quickjs,
                rnquickjs::QuickJSContext& context) {
          const auto started = std::chrono::steady_clock::now();
          rnquickjs::ExecutionResult result;
          try {
            const auto handle = global
                ? context.retainGlobal(source)
                : context.retainEvaluation(source, filename);
            result.value = rnquickjs::Value{static_cast<double>(handle)};
          } catch (const rnquickjs::QuickJSExecutionException& error) {
            result.reason = error.reason();
            result.code = error.code();
            result.error = error.error();
          } catch (const std::exception& error) {
            result.reason = "runtime";
            result.code = 1;
            result.error.name = "Error";
            result.error.message = error.what();
          }
          result.durationMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
          result.memory = quickjs.resultMemoryStats();
          result.outputTruncated = context.outputWasTruncated();
          return result;
        });
        return jsi::Value(static_cast<double>(taskId));
      });
    }

    if (property == "startCall") {
      return makeFunction(runtime, "startCall", 2, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value* args,
          std::size_t count) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto handle = requiredHandle(rt, args, count, 0);
        std::vector<rnquickjs::Value> values;
        if (count > 1 && !args[1].isUndefined()) {
          if (!args[1].isObject() ||
              !args[1].asObject(rt).isArray(rt)) {
            throw jsi::JSError(rt, "QuickJS call arguments must be an array");
          }
          const auto array = args[1].asObject(rt).asArray(rt);
          const auto size = array.size(rt);
          values.reserve(size);
          std::size_t nodes = 0;
          JSIBridgeHelpers helpers;
          for (std::size_t index = 0; index < size; ++index) {
            values.push_back(fromJSI(
                rt,
                array.getValueAtIndex(rt, index),
                0,
                nodes,
                helpers));
          }
        }

        const auto taskId = self->startTask([
            handle,
            values = std::move(values)](
                rnquickjs::QuickJSRuntime&,
                rnquickjs::QuickJSContext& context) mutable {
          return context.callAwaited(handle, values);
        });
        return jsi::Value(static_cast<double>(taskId));
      });
    }

    if (property == "startMemory") {
      return makeFunction(runtime, "startMemory", 0, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value*,
          std::size_t) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto taskId = self->startTask([](
            rnquickjs::QuickJSRuntime& quickjs,
            rnquickjs::QuickJSContext& context) {
          rnquickjs::ExecutionResult result;
          result.memory = quickjs.memoryStats();
          result.outputTruncated = context.outputWasTruncated();
          return result;
        });
        return jsi::Value(static_cast<double>(taskId));
      });
    }

    if (property == "takeTaskResult") {
      return makeFunction(runtime, "takeTaskResult", 1, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value* args,
          std::size_t count) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto taskId = requiredHandle(rt, args, count, 0);
        auto result = self->takeResult(taskId);
        if (!result.has_value()) {
          return jsi::Value::null();
        }
        auto object = resultToJSI(rt, result->result);
        object.setProperty(
            rt,
            "output",
            jsi::String::createFromUtf8(rt, result->output));
        return object;
      });
    }

    if (property == "release") {
      return makeFunction(runtime, "release", 1, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value* args,
          std::size_t count) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto handle = requiredHandle(rt, args, count, 0);
        self->enqueue([handle](
            rnquickjs::QuickJSRuntime&,
            rnquickjs::QuickJSContext& context) {
          context.release(handle);
        });
        return jsi::Value::undefined();
      });
    }

    if (property == "registerAsyncHostFunction") {
      return makeFunction(runtime, "registerAsyncHostFunction", 2, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value* args,
          std::size_t count) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto functionName =
            requiredString(rt, args, count, 0, "Host function name");
        if (functionName.empty()) {
          throw jsi::JSError(rt, "Host function name cannot be empty");
        }
        if (count < 2 || !args[1].isObject() ||
            !args[1].asObject(rt).isFunction(rt)) {
          throw jsi::JSError(
              rt, "Async host callback must be a function");
        }

        auto callback = std::make_shared<jsi::Function>(
            args[1].asObject(rt).asFunction(rt));
        auto* hostRuntime = &self->hostRuntime_;
        const auto invoker = self->callInvoker_;

        WorkerCallbackRegistry* registry = nullptr;
        jsi::Function* callbackPointer = nullptr;
        {
          std::lock_guard<std::mutex> lock(self->mutex_);
          if (self->stopping_ ||
              self->disposed_.load(std::memory_order_acquire) ||
              self->callbackRegistry_ == nullptr) {
            throw jsi::JSError(rt, "QuickJS worker is disposed");
          }
          registry = self->callbackRegistry_;
          registry->callbacks.push_back(callback);
          callbackPointer = callback.get();
        }

        std::uint64_t taskId = 0;
        try {
          taskId = self->startTask([
              functionName,
              hostRuntime,
              invoker,
              registry,
              callbackPointer](
                  rnquickjs::QuickJSRuntime&,
                  rnquickjs::QuickJSContext& context) {
            context.registerAsyncHostFunction(
                functionName,
                [hostRuntime, invoker, registry, callbackPointer](
                    const std::vector<rnquickjs::Value>& values,
                    rnquickjs::QuickJSContext::AsyncHostCompletion completion) {
                  invokeAsyncOnJSThread(
                      *hostRuntime,
                      invoker,
                      registry,
                      callbackPointer,
                      values,
                      std::move(completion));
                });
            return rnquickjs::ExecutionResult{};
          }, callbackPointer);
        } catch (...) {
          releaseWorkerCallback(registry, callbackPointer);
          throw;
        }
        return jsi::Value(static_cast<double>(taskId));
      });
    }

    if (property == "addModule") {
      return makeFunction(runtime, "addModule", 2, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value* args,
          std::size_t count) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto moduleName =
            requiredModuleName(rt, args, count, 0);
        const auto source =
            requiredString(rt, args, count, 1, "Module source");
        self->enqueue([
            moduleName,
            source](
                rnquickjs::QuickJSRuntime& quickjs,
                rnquickjs::QuickJSContext&) {
          quickjs.addModule(moduleName, source);
        });
        return jsi::Value::undefined();
      });
    }

    if (property == "removeModule") {
      return makeFunction(runtime, "removeModule", 1, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value* args,
          std::size_t count) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        const auto moduleName =
            requiredModuleName(rt, args, count, 0);
        self->enqueue([moduleName](
            rnquickjs::QuickJSRuntime& quickjs,
            rnquickjs::QuickJSContext&) {
          quickjs.removeModule(moduleName);
        });
        return jsi::Value::undefined();
      });
    }

    if (property == "clearModules") {
      return makeFunction(runtime, "clearModules", 0, [weakSelf](
          jsi::Runtime& rt,
          const jsi::Value&,
          const jsi::Value*,
          std::size_t) -> jsi::Value {
        const auto self = weakSelf.lock();
        if (!self) {
          throw jsi::JSError(rt, "QuickJS worker is unavailable");
        }
        self->enqueue([](
            rnquickjs::QuickJSRuntime& quickjs,
            rnquickjs::QuickJSContext&) {
          quickjs.clearModules();
        });
        return jsi::Value::undefined();
      });
    }

    if (property == "cancel") {
      return makeFunction(runtime, "cancel", 0, [weakSelf](
          jsi::Runtime&,
          const jsi::Value&,
          const jsi::Value*,
          std::size_t) -> jsi::Value {
        if (const auto self = weakSelf.lock()) {
          self->cancellationGeneration_.fetch_add(
              1, std::memory_order_acq_rel);
          self->requestActiveRuntimeCancellation();
        }
        return jsi::Value::undefined();
      });
    }

    if (property == "dispose") {
      return makeFunction(runtime, "dispose", 0, [weakSelf](
          jsi::Runtime&,
          const jsi::Value&,
          const jsi::Value*,
          std::size_t) -> jsi::Value {
        if (const auto self = weakSelf.lock()) {
          self->shutdown();
        }
        return jsi::Value::undefined();
      });
    }

    return jsi::Value::undefined();
  }

  std::vector<jsi::PropNameID> getPropertyNames(
      jsi::Runtime& runtime) override {
    static constexpr const char* names[] = {
        "valid",
        "executing",
        "startEvaluate",
        "startRetain",
        "startCall",
        "startMemory",
        "takeTaskResult",
        "release",
        "registerAsyncHostFunction",
        "addModule",
        "removeModule",
        "clearModules",
        "cancel",
        "dispose"};
    std::vector<jsi::PropNameID> result;
    result.reserve(sizeof(names) / sizeof(names[0]));
    for (const char* item : names) {
      result.push_back(jsi::PropNameID::forAscii(runtime, item));
    }
    return result;
  }

 private:
  std::uint64_t startTask(
      std::function<rnquickjs::ExecutionResult(
          rnquickjs::QuickJSRuntime&,
          rnquickjs::QuickJSContext&)> operation,
      jsi::Function* callbackToReleaseOnFailure = nullptr) {
    const auto previousOutstanding =
        outstandingTasks_.fetch_add(1, std::memory_order_acq_rel);
    if (previousOutstanding >= kMaxWorkerOutstandingTasks) {
      outstandingTasks_.fetch_sub(1, std::memory_order_acq_rel);
      throw std::runtime_error(
          "QuickJS worker has too many outstanding tasks");
    }

    const std::uint64_t taskId = nextTaskId_++;
    const std::uint64_t cancellationGeneration =
        cancellationGeneration_.load(std::memory_order_acquire);

    try {
      enqueue([
          this,
          taskId,
          cancellationGeneration,
          callbackToReleaseOnFailure,
          operation = std::move(operation)](
              rnquickjs::QuickJSRuntime& quickjs,
              rnquickjs::QuickJSContext& context) mutable {
        executing_.store(true, std::memory_order_release);

        rnquickjs::ExecutionResult result;
        const auto markCancelled = [&] {
          result.reason = "cancelled";
          result.code = 1001;
          result.error.name = "InternalError";
          result.error.message = "Execution cancelled";
          result.memory = quickjs.resultMemoryStats();
          result.outputTruncated = context.outputWasTruncated();
        };
        const auto markDestroyed = [&] {
          result.reason = "destroyed";
          result.code = 1003;
          result.error.name = "Error";
          result.error.message = "QuickJS worker is disposed";
          result.memory = quickjs.resultMemoryStats();
          result.outputTruncated = context.outputWasTruncated();
        };

        try {
          if (disposed_.load(std::memory_order_acquire)) {
            markDestroyed();
          } else {
            // Clear cancellation left by an earlier task, then immediately
            // re-check the generation. A cancel that raced just before this
            // reset is therefore not lost.
            quickjs.resetCancellation();
            if (disposed_.load(std::memory_order_acquire)) {
              quickjs.requestCancellation();
              markDestroyed();
            } else if (
                cancellationGeneration_.load(std::memory_order_acquire) !=
                cancellationGeneration) {
              quickjs.requestCancellation();
              markCancelled();
            } else {
              result = operation(quickjs, context);

              // Some operations publish persistent native state before the
              // task-boundary checkpoint runs. In particular, a successful
              // host-function registration makes QuickJS retain a raw callback
              // pointer. Commit that ownership now; a later microtask failure
              // may change the task result, but must not free installed state.
              if (result.ok() && callbackToReleaseOnFailure != nullptr) {
                callbackToReleaseOnFailure = nullptr;
              }

              // Worker tasks are task-isolated at the same-turn microtask
              // boundary. Low-level QuickJSContext calls keep their existing
              // behavior/performance; the worker pays only a cheap pending-job
              // check when no microtasks were queued.
              if (JS_IsJobPending(quickjs.rawRuntime())) {
                auto jobs = context.executePendingJobs(
                    std::numeric_limits<std::size_t>::max(), false);
                if (result.ok() && !jobs.ok()) {
                  result = std::move(jobs);
                }
              }
              result.outputTruncated = context.outputWasTruncated();
            }
          }
        } catch (const std::exception& error) {
          result.reason = "runtime";
          result.code = 1;
          result.error.name = "Error";
          result.error.message = error.what();
          result.memory = quickjs.resultMemoryStats();
          result.outputTruncated = context.outputWasTruncated();
        } catch (...) {
          result.reason = "runtime";
          result.code = 1;
          result.error.name = "Error";
          result.error.message = "Unknown QuickJS worker failure";
          result.memory = quickjs.resultMemoryStats();
          result.outputTruncated = context.outputWasTruncated();
        }

        auto output = context.takeOutput();
        {
          std::lock_guard<std::mutex> lock(mutex_);
          results_.push_back(WorkerTaskResult{
              taskId,
              std::move(result),
              std::move(output),
              callbackToReleaseOnFailure});
        }
        executing_.store(false, std::memory_order_release);
        condition_.notify_all();
      });
    } catch (...) {
      outstandingTasks_.fetch_sub(1, std::memory_order_acq_rel);
      throw;
    }
    return taskId;
  }

  void enqueue(Command command) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || disposed_.load(std::memory_order_relaxed)) {
        throw std::runtime_error("QuickJS worker is disposed");
      }
      commands_.push_back(std::move(command));
    }
    condition_.notify_one();
  }

  std::optional<WorkerTaskResult> takeResult(std::uint64_t taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(
        results_.begin(),
        results_.end(),
        [taskId](const WorkerTaskResult& result) {
          return result.taskId == taskId;
        });
    if (found == results_.end()) {
      return std::nullopt;
    }
    WorkerTaskResult result = std::move(*found);
    results_.erase(found);
    if (!result.result.ok() &&
        result.callbackToReleaseOnFailure != nullptr) {
      releaseWorkerCallback(
          callbackRegistry_, result.callbackToReleaseOnFailure);
      result.callbackToReleaseOnFailure = nullptr;
    }
    outstandingTasks_.fetch_sub(1, std::memory_order_acq_rel);
    return result;
  }

  void setActiveRuntime(rnquickjs::QuickJSRuntime* runtime) noexcept {
    std::lock_guard<std::mutex> lock(activeRuntimeMutex_);
    activeRuntime_ = runtime;
  }

  void requestActiveRuntimeCancellation() noexcept {
    std::lock_guard<std::mutex> lock(activeRuntimeMutex_);
    if (activeRuntime_ != nullptr) {
      activeRuntime_->requestCancellation();
    }
  }

  void workerMain() noexcept {
    try {
      rnquickjs::QuickJSRuntime quickjs(options_);
      auto context = quickjs.createContext();
      setActiveRuntime(&quickjs);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
      }
      condition_.notify_all();

      while (true) {
        Command command;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          condition_.wait(lock, [this] {
            return stopping_ || !commands_.empty();
          });
          if (stopping_ && commands_.empty()) {
            break;
          }
          command = std::move(commands_.front());
          commands_.pop_front();
        }

        try {
          command(quickjs, *context);
        } catch (...) {
          // Task operations translate ordinary QuickJS/host failures into
          // results themselves. Reaching this boundary means delivery or a
          // configuration command failed unexpectedly (for example host OOM).
          // Stop the worker so JS pollers observe valid=false instead of
          // waiting forever for a result that can no longer be delivered.
          executing_.store(false, std::memory_order_release);
          failed_.store(true, std::memory_order_release);
          {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            commands_.clear();
          }
          condition_.notify_all();
        }
      }

      setActiveRuntime(nullptr);
      context->dispose();
      quickjs.dispose();
    } catch (const std::exception& error) {
      setActiveRuntime(nullptr);
      std::lock_guard<std::mutex> lock(mutex_);
      startupError_ = error.what();
      ready_ = true;
      stopping_ = true;
      condition_.notify_all();
    } catch (...) {
      setActiveRuntime(nullptr);
      std::lock_guard<std::mutex> lock(mutex_);
      startupError_ = "Unknown QuickJS worker startup failure";
      ready_ = true;
      stopping_ = true;
      condition_.notify_all();
    }
  }

  void shutdown() noexcept {
    if (disposed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    cancellationGeneration_.fetch_add(
        1, std::memory_order_acq_rel);
    requestActiveRuntimeCancellation();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      // Serialize registry shutdown with callback registration so a JS call
      // racing finalization cannot observe or retain a freed registry.
      if (callbackRegistry_ != nullptr) {
        callbackRegistry_->shutdown.store(true, std::memory_order_release);
      }
      commands_.clear();
    }
    condition_.notify_all();
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
      worker_.join();
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      results_.clear();
    }
    outstandingTasks_.store(0, std::memory_order_release);

    // Hermes may finalize HostObjects on a GC thread. Keep all retained
    // jsi::Function ownership in a heap registry whose final deletion is
    // guaranteed to happen on the JS thread. If RN drops queued CallInvoker
    // work during teardown, the registry intentionally leaks rather than
    // releasing JSI values from the wrong thread.
    auto* registry = std::exchange(callbackRegistry_, nullptr);
    if (registry != nullptr) {
      const auto previous =
          registry->pendingDispatches.fetch_sub(
              1, std::memory_order_acq_rel);
      if (previous == 1) {
        // No JS dispatch owns the registry. The owner reference is the final
        // one, so release it only on the JS thread.
        if (std::this_thread::get_id() == jsThread_) {
          delete registry;
        } else if (callInvoker_) {
          try {
            callInvoker_->invokeAsync([registry]() {
              delete registry;
            });
          } catch (...) {
            // JS executor already unavailable: intentionally leak.
          }
        }
      }
      // Otherwise a queued JS dispatch still owns a reference. The final
      // dispatch to finish will delete the registry on the JS thread.
    }
  }

  jsi::Runtime& hostRuntime_;
  std::shared_ptr<CallInvoker> callInvoker_;
  rnquickjs::RuntimeOptions options_;
  std::thread worker_;
  std::thread::id jsThread_{std::this_thread::get_id()};

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Command> commands_;
  std::deque<WorkerTaskResult> results_;
  WorkerCallbackRegistry* callbackRegistry_ = nullptr;

  std::mutex activeRuntimeMutex_;
  rnquickjs::QuickJSRuntime* activeRuntime_ = nullptr;
  std::atomic<bool> executing_{false};
  std::atomic<bool> disposed_{false};
  std::atomic<bool> failed_{false};
  std::atomic<std::uint64_t> cancellationGeneration_{0};
  std::atomic<std::size_t> outstandingTasks_{0};
  bool ready_ = false;
  bool stopping_ = false;
  std::string startupError_;
  std::uint64_t nextTaskId_ = 1;
};

class RuntimeHostObject;

class ContextHostObject final : public jsi::HostObject,
                                public std::enable_shared_from_this<ContextHostObject> {
 public:
  ContextHostObject(
      jsi::Runtime& hostRuntime,
      std::shared_ptr<CallInvoker> callInvoker,
      std::shared_ptr<RuntimeHostObject> owner,
      std::shared_ptr<rnquickjs::QuickJSContext> context)
      : hostRuntime_(hostRuntime),
        callInvoker_(std::move(callInvoker)),
        owner_(std::move(owner)),
        context_(std::move(context)),
        jsThread_(std::this_thread::get_id()) {}

  ~ContextHostObject() override {
    if (!context_) {
      return;
    }

    // Hermes Hades may finalize HostObjects away from the JS thread. QuickJS
    // contexts in the synchronous embedding path are JS-thread-affine, and
    // disposing also releases retained JSI host callbacks.
    if (callInvoker_ && std::this_thread::get_id() != jsThread_) {
      struct DeferredContextCleanup {
        std::shared_ptr<rnquickjs::QuickJSContext> context;
        std::shared_ptr<RuntimeHostObject> owner;
      };
      auto* deferred = new DeferredContextCleanup{
          std::move(context_), std::move(owner_)};
      try {
        callInvoker_->invokeAsync([deferred]() {
          deferred->context->dispose();
          delete deferred;
        });
      } catch (...) {
        // If the JS executor is already gone, intentionally leak the payload
        // rather than destroy QuickJS state on a Hermes GC thread.
      }
      return;
    }
    context_->dispose();
  }

  jsi::Value get(jsi::Runtime& runtime, const jsi::PropNameID& name) override;

  std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime& runtime) override {
    static constexpr const char* names[] = {
        "valid", "outputCount", "outputTruncated", "evaluate", "retain",
        "call", "release", "executePendingJobs", "getOutput", "takeOutput",
        "registerHostFunction", "dispose"};
    std::vector<jsi::PropNameID> result;
    result.reserve(sizeof(names) / sizeof(names[0]));
    for (const char* item : names) {
      result.push_back(jsi::PropNameID::forAscii(runtime, item));
    }
    return result;
  }

 private:
  rnquickjs::QuickJSContext& requireContext(jsi::Runtime& runtime) const {
    if (!context_ || !context_->isOpen()) {
      throw jsi::JSError(runtime, "QuickJS context is disposed");
    }
    return *context_;
  }

  jsi::Runtime& hostRuntime_;
  std::shared_ptr<CallInvoker> callInvoker_;
  std::shared_ptr<RuntimeHostObject> owner_;
  std::shared_ptr<rnquickjs::QuickJSContext> context_;
  std::thread::id jsThread_;
};

class RuntimeHostObject final : public jsi::HostObject,
                                public std::enable_shared_from_this<RuntimeHostObject> {
 public:
  RuntimeHostObject(
      std::shared_ptr<CallInvoker> callInvoker,
      rnquickjs::RuntimeOptions options)
      : callInvoker_(std::move(callInvoker)),
        runtime_(std::make_unique<rnquickjs::QuickJSRuntime>(options)),
        jsThread_(std::this_thread::get_id()) {}

  ~RuntimeHostObject() override {
    if (!runtime_) {
      return;
    }

    if (callInvoker_ && std::this_thread::get_id() != jsThread_) {
      auto* deferred =
          new std::unique_ptr<rnquickjs::QuickJSRuntime>(
              std::move(runtime_));
      try {
        callInvoker_->invokeAsync([deferred]() {
          (*deferred)->dispose();
          delete deferred;
        });
      } catch (...) {
        // If the JS executor is already gone, intentionally leak the payload
        // rather than destroy QuickJS state on a Hermes GC thread.
      }
      return;
    }
    runtime_->dispose();
  }

  jsi::Value get(jsi::Runtime& runtime, const jsi::PropNameID& name) override;

  std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime& runtime) override {
    static constexpr const char* names[] = {
        "valid", "executionLimitMs", "memoryLimitBytes", "maxStackBytes",
        "memory", "createContext", "addModule", "removeModule", "clearModules",
        "cancel", "resetCancellation", "setExecutionLimitMs",
        "setMemoryLimitBytes", "setMaxStackBytes", "dispose"};
    std::vector<jsi::PropNameID> result;
    result.reserve(sizeof(names) / sizeof(names[0]));
    for (const char* item : names) {
      result.push_back(jsi::PropNameID::forAscii(runtime, item));
    }
    return result;
  }

 private:
  rnquickjs::QuickJSRuntime& requireRuntime(jsi::Runtime& runtime) const {
    if (!runtime_ || !runtime_->isOpen()) {
      throw jsi::JSError(runtime, "QuickJS runtime is disposed");
    }
    return *runtime_;
  }

  std::shared_ptr<CallInvoker> callInvoker_;
  std::unique_ptr<rnquickjs::QuickJSRuntime> runtime_;
  std::thread::id jsThread_;
};

jsi::Value RuntimeHostObject::get(
    jsi::Runtime& runtime,
    const jsi::PropNameID& name) {
  const auto property = name.utf8(runtime);
  if (property == "valid") {
    return jsi::Value(runtime_ && runtime_->isOpen());
  }
  if (property == "executionLimitMs") {
    return jsi::Value(static_cast<double>(
        runtime_ ? runtime_->executionLimitMs() : 0));
  }
  if (property == "memoryLimitBytes") {
    return jsi::Value(static_cast<double>(
        runtime_ ? runtime_->memoryLimitBytes() : 0));
  }
  if (property == "maxStackBytes") {
    return jsi::Value(static_cast<double>(
        runtime_ ? runtime_->maxStackBytes() : 0));
  }
  if (property == "memory") {
    return memoryToJSI(
        runtime, runtime_ ? runtime_->memoryStats() : rnquickjs::MemoryStats{});
  }

  const std::weak_ptr<RuntimeHostObject> weakSelf = shared_from_this();
  if (property == "createContext") {
    return makeFunction(runtime, "createContext", 0, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, std::size_t) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS runtime is unavailable");
      }
      auto context = std::make_shared<ContextHostObject>(
          rt, self->callInvoker_, self, self->requireRuntime(rt).createContext());
      return jsi::Object::createFromHostObject(rt, std::move(context));
    });
  }
  if (property == "addModule") {
    return makeFunction(runtime, "addModule", 2, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS runtime is unavailable");
      }
      self->requireRuntime(rt).addModule(
          requiredModuleName(rt, args, count, 0),
          requiredString(rt, args, count, 1, "Module source"));
      return jsi::Value::undefined();
    });
  }
  if (property == "removeModule" || property == "clearModules") {
    return makeFunction(runtime, property.c_str(), property == "removeModule" ? 1 : 0,
        [weakSelf, property](jsi::Runtime& rt, const jsi::Value&,
                            const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS runtime is unavailable");
      }
      auto& quickjs = self->requireRuntime(rt);
      if (property == "removeModule") {
        quickjs.removeModule(requiredModuleName(rt, args, count, 0));
      } else {
        quickjs.clearModules();
      }
      return jsi::Value::undefined();
    });
  }
  if (property == "cancel" || property == "resetCancellation") {
    return makeFunction(runtime, property.c_str(), 0, [weakSelf, property](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, std::size_t) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS runtime is unavailable");
      }
      auto& quickjs = self->requireRuntime(rt);
      if (property == "cancel") {
        quickjs.requestCancellation();
      } else {
        quickjs.resetCancellation();
      }
      return jsi::Value::undefined();
    });
  }
  if (property == "setExecutionLimitMs" ||
      property == "setMemoryLimitBytes" || property == "setMaxStackBytes") {
    return makeFunction(runtime, property.c_str(), 1, [weakSelf, property](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS runtime is unavailable");
      }
      if (count == 0 || !args[0].isNumber() ||
          !std::isfinite(args[0].asNumber()) || args[0].asNumber() < 0) {
        throw jsi::JSError(rt, "QuickJS limit must be a non-negative finite number");
      }
      const double requested = args[0].asNumber();
      auto& quickjs = self->requireRuntime(rt);
      if (property == "setExecutionLimitMs") {
        quickjs.setExecutionLimitMs(
            requested <= 0
                ? 0
                : static_cast<std::int64_t>(
                      std::ceil(std::min(requested, kMaxExecutionLimitMs))));
      } else {
        if (requested > kMaxSizeValue) {
          throw jsi::JSError(rt, "QuickJS byte limit exceeds the native size range");
        }
        if (property == "setMemoryLimitBytes") {
          quickjs.setMemoryLimitBytes(static_cast<std::size_t>(requested));
        } else {
          quickjs.setMaxStackBytes(static_cast<std::size_t>(requested));
        }
      }
      return jsi::Value::undefined();
    });
  }
  if (property == "dispose") {
    return makeFunction(runtime, "dispose", 0, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, std::size_t) {
      if (const auto self = weakSelf.lock(); self && self->runtime_) {
        if (!self->runtime_->isOpen()) {
          return jsi::Value::undefined();
        }
        auto& quickjs = *self->runtime_;
        if (quickjs.isExecuting()) {
          throw jsi::JSError(
              rt, "Cannot dispose a QuickJS runtime while it is executing");
        }
        quickjs.dispose();
      }
      return jsi::Value::undefined();
    });
  }
  return jsi::Value::undefined();
}

jsi::Value ContextHostObject::get(
    jsi::Runtime& runtime,
    const jsi::PropNameID& name) {
  const auto property = name.utf8(runtime);
  if (property == "valid") {
    return jsi::Value(context_ && context_->isOpen());
  }
  if (property == "outputCount") {
    return jsi::Value(static_cast<double>(
        context_ && context_->isOpen() ? context_->outputCount() : 0));
  }
  if (property == "outputTruncated") {
    return jsi::Value(
        context_ && context_->isOpen() && context_->outputWasTruncated());
  }

  const std::weak_ptr<ContextHostObject> weakSelf = shared_from_this();
  if (property == "evaluate") {
    return makeFunction(runtime, "evaluate", 2, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS context is unavailable");
      }
      const auto source = requiredString(rt, args, count, 0, "Source");
      std::string filename = "<eval>";
      auto mode = rnquickjs::EvalMode::Script;
      if (count > 1 && !args[1].isUndefined()) {
        if (!args[1].isObject()) {
          throw jsi::JSError(rt, "Evaluate options must be an object");
        }
        const auto options = args[1].asObject(rt);
        if (!isPlainObject(rt, options)) {
          throw jsi::JSError(rt, "Evaluate options must be a plain object");
        }
        const auto filenameValue = options.getProperty(rt, "filename");
        if (!filenameValue.isUndefined()) {
          if (!filenameValue.isString()) {
            throw jsi::JSError(rt, "Evaluate filename must be a string");
          }
          filename = filenameValue.asString(rt).utf8(rt);
          validateFilename(rt, filename);
        }
        mode = evalMode(rt, options);
      }
      return resultToJSI(rt, self->requireContext(rt).evaluate(source, filename, mode));
    });
  }
  if (property == "retain") {
    return makeFunction(runtime, "retain", 2, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS context is unavailable");
      }
      const auto source = requiredString(rt, args, count, 0, "Retained source or global");
      bool global = false;
      std::string filename = "<retain>";
      if (count > 1 && !args[1].isUndefined()) {
        if (!args[1].isObject()) {
          throw jsi::JSError(rt, "Retain options must be an object");
        }
        const auto options = args[1].asObject(rt);
        if (!isPlainObject(rt, options)) {
          throw jsi::JSError(rt, "Retain options must be a plain object");
        }
        const auto globalValue = options.getProperty(rt, "global");
        if (!globalValue.isUndefined()) {
          if (!globalValue.isBool()) {
            throw jsi::JSError(rt, "Retain global must be a boolean");
          }
          global = globalValue.getBool();
        }
        const auto filenameValue = options.getProperty(rt, "filename");
        if (!filenameValue.isUndefined()) {
          if (!filenameValue.isString()) {
            throw jsi::JSError(rt, "Retain filename must be a string");
          }
          filename = filenameValue.asString(rt).utf8(rt);
          validateFilename(rt, filename);
        }
      }
      auto& quickjs = self->requireContext(rt);
      const auto handle = global ? quickjs.retainGlobal(source)
                                 : quickjs.retainEvaluation(source, filename);
      return jsi::Value(static_cast<double>(handle));
    });
  }
  if (property == "call") {
    return makeFunction(runtime, "call", 2, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS context is unavailable");
      }
      const auto handle = requiredHandle(rt, args, count, 0);
      std::vector<rnquickjs::Value> values;
      if (count > 1 && !args[1].isUndefined()) {
        if (!args[1].isObject() || !args[1].asObject(rt).isArray(rt)) {
          throw jsi::JSError(rt, "QuickJS call arguments must be an array");
        }
        const auto array = args[1].asObject(rt).asArray(rt);
        const auto size = array.size(rt);
        values.reserve(size);
        std::size_t nodes = 0;
        JSIBridgeHelpers helpers;
        for (std::size_t index = 0; index < size; ++index) {
          const auto value = array.getValueAtIndex(rt, index);
          values.push_back(fromJSI(rt, value, 0, nodes, helpers));
        }
      }
      return resultToJSI(rt, self->requireContext(rt).call(handle, values));
    });
  }
  if (property == "release") {
    return makeFunction(runtime, "release", 1, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS context is unavailable");
      }
      self->requireContext(rt).release(requiredHandle(rt, args, count, 0));
      return jsi::Value::undefined();
    });
  }
  if (property == "executePendingJobs") {
    return makeFunction(runtime, "executePendingJobs", 1, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS context is unavailable");
      }
      std::size_t maxJobs = 1'000;
      if (count > 0 && !args[0].isUndefined()) {
        if (!args[0].isNumber() || !std::isfinite(args[0].asNumber()) ||
            args[0].asNumber() < 0 || args[0].asNumber() > kMaxSizeValue) {
          throw jsi::JSError(
              rt, "maxJobs must fit in the native size range");
        }
        maxJobs = static_cast<std::size_t>(args[0].asNumber());
      }
      return resultToJSI(rt, self->requireContext(rt).executePendingJobs(maxJobs));
    });
  }
  if (property == "getOutput" || property == "takeOutput") {
    return makeFunction(runtime, property.c_str(), 1, [weakSelf, property](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS context is unavailable");
      }
      std::size_t requested = 0;
      if (count > 0 && !args[0].isUndefined()) {
        if (!args[0].isNumber() || !std::isfinite(args[0].asNumber()) ||
            args[0].asNumber() < 0 || args[0].asNumber() > kMaxSizeValue) {
          throw jsi::JSError(
              rt, "Output count must fit in the native size range");
        }
        requested = static_cast<std::size_t>(args[0].asNumber());
      }
      auto& quickjs = self->requireContext(rt);
      const auto output = property == "getOutput"
          ? quickjs.getOutput(requested) : quickjs.takeOutput(requested);
      return jsi::String::createFromUtf8(rt, output);
    });
  }
  if (property == "registerHostFunction") {
    return makeFunction(runtime, "registerHostFunction", 2, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, std::size_t count) {
      const auto self = weakSelf.lock();
      if (!self) {
        throw jsi::JSError(rt, "QuickJS context is unavailable");
      }
      const auto functionName = requiredString(rt, args, count, 0, "Host function name");
      if (count < 2 || !args[1].isObject() ||
          !args[1].asObject(rt).isFunction(rt)) {
        throw jsi::JSError(rt, "Host callback must be a function");
      }
      auto callback = std::make_shared<jsi::Function>(
          args[1].asObject(rt).asFunction(rt));
      auto* hostRuntime = &self->hostRuntime_;
      const auto invoker = self->callInvoker_;
      const auto jsThread = self->jsThread_;
      self->requireContext(rt).registerHostFunction(
          functionName,
          [hostRuntime, invoker, jsThread, callback](
              const std::vector<rnquickjs::Value>& values) {
            return invokeOnJSThread(*hostRuntime, invoker, jsThread, callback, values);
          });
      return jsi::Value::undefined();
    });
  }
  if (property == "dispose") {
    return makeFunction(runtime, "dispose", 0, [weakSelf](
        jsi::Runtime& rt, const jsi::Value&, const jsi::Value*, std::size_t) {
      if (const auto self = weakSelf.lock(); self && self->context_) {
        if (!self->context_->isOpen()) {
          return jsi::Value::undefined();
        }
        auto& quickjs = *self->context_;
        if (!quickjs.canDispose()) {
          throw jsi::JSError(
              rt, "Cannot dispose a QuickJS context while it is executing");
        }
        quickjs.dispose();
      }
      return jsi::Value::undefined();
    });
  }
  return jsi::Value::undefined();
}

} // namespace

void install(
    facebook::jsi::Runtime& runtime,
    std::shared_ptr<facebook::react::CallInvoker> callInvoker) {
  const auto runtimeInvoker = callInvoker;
  auto factory = makeFunction(runtime, kFactoryName, 1, [runtimeInvoker](
      jsi::Runtime& hostRuntime, const jsi::Value&, const jsi::Value* arguments,
      std::size_t count) {
    if (!runtimeInvoker) {
      throw jsi::JSError(
          hostRuntime, "React Native CallInvoker is unavailable");
    }
    auto hostObject = std::make_shared<RuntimeHostObject>(
        runtimeInvoker, runtimeOptions(hostRuntime, arguments, count));
    return jsi::Object::createFromHostObject(hostRuntime, std::move(hostObject));
  });
  runtime.global().setProperty(runtime, kFactoryName, std::move(factory));

  auto workerFactory = makeFunction(
      runtime,
      kWorkerFactoryName,
      1,
      [callInvoker = std::move(callInvoker)](
          jsi::Runtime& hostRuntime,
          const jsi::Value&,
          const jsi::Value* arguments,
          std::size_t count) {
        if (!callInvoker) {
          throw jsi::JSError(
              hostRuntime, "React Native CallInvoker is unavailable");
        }
        auto worker = std::make_shared<WorkerHostObject>(
            hostRuntime,
            callInvoker,
            runtimeOptions(hostRuntime, arguments, count));
        return jsi::Object::createFromHostObject(
            hostRuntime, std::move(worker));
      });
  runtime.global().setProperty(
      runtime, kWorkerFactoryName, std::move(workerFactory));
}

void cleanup(facebook::jsi::Runtime& runtime) {
  runtime.global().setProperty(
      runtime, kFactoryName, facebook::jsi::Value::undefined());
  runtime.global().setProperty(
      runtime, kWorkerFactoryName, facebook::jsi::Value::undefined());
}

} // namespace SKRNNativeQuickJS
