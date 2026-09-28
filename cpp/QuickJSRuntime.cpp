#include "QuickJSRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rnquickjs {
namespace {

constexpr std::size_t kMinMemoryLimit = 512 * 1024;
constexpr std::size_t kMaxMemoryLimit = 512 * 1024 * 1024;
constexpr std::size_t kMinStackSize = 64 * 1024;
constexpr std::size_t kMaxStackSize = 32 * 1024 * 1024;
constexpr std::int64_t kMaxExecutionLimitMs = 5 * 60 * 1000;
constexpr std::size_t kMaxOutputBytes = 1024 * 1024;
constexpr std::size_t kMaxOutputLines = 10'000;
constexpr int kMaxValueDepth = 32;
constexpr std::size_t kMaxValueNodes = 16'384;

std::int64_t nowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string toString(JSContext* context, JSValueConst value) {
  size_t length = 0;
  const char* text = JS_ToCStringLen(context, &length, value);
  if (text == nullptr) {
    return {};
  }
  std::string result(text, length);
  JS_FreeCString(context, text);
  return result;
}

std::string stringProperty(
    JSContext* context,
    JSValueConst object,
    const char* name) {
  JSValue value = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(value)) {
    return {};
  }
  std::string result;
  if (!JS_IsUndefined(value) && !JS_IsNull(value)) {
    result = toString(context, value);
  }
  JS_FreeValue(context, value);
  return result;
}

std::size_t nonNegative(int64_t value) noexcept {
  return value <= 0 ? 0 : static_cast<std::size_t>(value);
}

int evalFlags(EvalMode mode) noexcept {
  switch (mode) {
    case EvalMode::Script:
      return JS_EVAL_TYPE_GLOBAL;
    case EvalMode::Module:
      return JS_EVAL_TYPE_MODULE;
    case EvalMode::AsyncScript:
      return JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC;
    case EvalMode::AsyncModule:
      return JS_EVAL_TYPE_MODULE;
  }
  return JS_EVAL_TYPE_GLOBAL;
}

} // namespace

QuickJSRuntime::QuickJSRuntime(RuntimeOptions options) : options_(options) {
  options_.executionLimitMs =
      std::clamp<std::int64_t>(options_.executionLimitMs, 1, kMaxExecutionLimitMs);
  options_.memoryLimitBytes =
      std::clamp<std::size_t>(options_.memoryLimitBytes, kMinMemoryLimit, kMaxMemoryLimit);
  options_.maxStackBytes =
      std::clamp<std::size_t>(options_.maxStackBytes, kMinStackSize, kMaxStackSize);
  options_.maxOutputBytes =
      std::min(options_.maxOutputBytes, kMaxOutputBytes);
  options_.maxOutputLines =
      std::min(options_.maxOutputLines, kMaxOutputLines);

  runtime_ = JS_NewRuntime();
  if (runtime_ == nullptr) {
    throw std::runtime_error("Unable to allocate QuickJS runtime");
  }

  JS_SetRuntimeOpaque(runtime_, this);
  JS_SetMemoryLimit(runtime_, options_.memoryLimitBytes);
  JS_SetMaxStackSize(runtime_, options_.maxStackBytes);
  JS_SetInterruptHandler(runtime_, &QuickJSRuntime::interruptHandler, this);
  JS_SetHostPromiseRejectionTracker(
      runtime_, &QuickJSRuntime::promiseRejectionTracker, this);
  JS_SetModuleLoaderFunc(
      runtime_, nullptr, &QuickJSRuntime::moduleLoader, this);
}

QuickJSRuntime::~QuickJSRuntime() {
  dispose();
}

std::shared_ptr<QuickJSContext> QuickJSRuntime::createContext() {
  if (!isOpen()) {
    throw std::runtime_error("QuickJS runtime is disposed");
  }
  auto context = std::make_shared<QuickJSContext>(*this);
  {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    contexts_.push_back(context);
  }
  return context;
}

void QuickJSRuntime::addModule(std::string name, std::string source) {
  if (name.empty()) {
    throw std::invalid_argument("Module name cannot be empty");
  }
  std::lock_guard<std::mutex> lock(moduleMutex_);
  modules_[std::move(name)] = std::move(source);
}

void QuickJSRuntime::removeModule(const std::string& name) {
  std::lock_guard<std::mutex> lock(moduleMutex_);
  modules_.erase(name);
}

void QuickJSRuntime::clearModules() {
  std::lock_guard<std::mutex> lock(moduleMutex_);
  modules_.clear();
}

void QuickJSRuntime::requestCancellation() noexcept {
  cancellationRequested_.store(true, std::memory_order_relaxed);
  std::vector<std::shared_ptr<QuickJSContext>> contexts;
  {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    contexts = contexts_;
  }
  for (const auto& context : contexts) {
    if (context) {
      context->notifyAsyncActivity();
    }
  }
}

void QuickJSRuntime::resetCancellation() noexcept {
  cancellationRequested_.store(false, std::memory_order_relaxed);
}

bool QuickJSRuntime::cancellationRequested() const noexcept {
  return cancellationRequested_.load(std::memory_order_relaxed);
}

void QuickJSRuntime::setExecutionLimitMs(std::int64_t value) noexcept {
  options_.executionLimitMs =
      std::clamp<std::int64_t>(value, 1, kMaxExecutionLimitMs);
}

std::int64_t QuickJSRuntime::executionLimitMs() const noexcept {
  return options_.executionLimitMs;
}

void QuickJSRuntime::setMemoryLimitBytes(std::size_t value) noexcept {
  options_.memoryLimitBytes =
      std::clamp<std::size_t>(value, kMinMemoryLimit, kMaxMemoryLimit);
  if (runtime_ != nullptr) {
    JS_SetMemoryLimit(runtime_, options_.memoryLimitBytes);
  }
}

std::size_t QuickJSRuntime::memoryLimitBytes() const noexcept {
  return options_.memoryLimitBytes;
}

void QuickJSRuntime::setMaxStackBytes(std::size_t value) noexcept {
  options_.maxStackBytes =
      std::clamp<std::size_t>(value, kMinStackSize, kMaxStackSize);
  if (runtime_ != nullptr) {
    JS_SetMaxStackSize(runtime_, options_.maxStackBytes);
  }
}

std::size_t QuickJSRuntime::maxStackBytes() const noexcept {
  return options_.maxStackBytes;
}

MemoryStats QuickJSRuntime::memoryStats() const noexcept {
  MemoryStats result;
  if (runtime_ == nullptr) {
    return result;
  }

  JSMemoryUsage usage{};
  JS_ComputeMemoryUsage(runtime_, &usage);
  result.mallocBytes = nonNegative(usage.malloc_size);
  result.memoryUsedBytes = nonNegative(usage.memory_used_size);
  result.mallocCount = nonNegative(usage.malloc_count);
  result.objectCount = nonNegative(usage.obj_count);
  result.atomCount = nonNegative(usage.atom_count);
  return result;
}

void QuickJSRuntime::dispose() noexcept {
  if (runtime_ == nullptr || isExecuting()) {
    return;
  }

  std::vector<std::shared_ptr<QuickJSContext>> contexts;
  {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    contexts = contexts_;
  }
  for (const auto& context : contexts) {
    if (context) {
      context->dispose();
    }
  }

  JS_SetInterruptHandler(runtime_, nullptr, nullptr);
  JS_SetHostPromiseRejectionTracker(runtime_, nullptr, nullptr);
  JS_SetModuleLoaderFunc(runtime_, nullptr, nullptr, nullptr);
  JS_SetRuntimeOpaque(runtime_, nullptr);
  JS_FreeRuntime(runtime_);
  runtime_ = nullptr;
}

bool QuickJSRuntime::isOpen() const noexcept {
  return runtime_ != nullptr;
}

bool QuickJSRuntime::isExecuting() const noexcept {
  return executionDepth_ > 0;
}

int QuickJSRuntime::interruptHandler(JSRuntime*, void* opaque) {
  auto* runtime = static_cast<QuickJSRuntime*>(opaque);
  if (runtime == nullptr) {
    return 0;
  }
  return runtime->cancellationRequested() || runtime->deadlineExceeded();
}

void QuickJSRuntime::promiseRejectionTracker(
    JSContext* context,
    JSValueConst promise,
    JSValueConst reason,
    int isHandled,
    void* opaque) {
  auto* runtime = static_cast<QuickJSRuntime*>(opaque);
  if (runtime == nullptr || !JS_IsObject(promise)) {
    return;
  }

  const void* identity = JS_VALUE_GET_PTR(promise);
  if (identity == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(runtime->rejectionMutex_);
  if (isHandled) {
    runtime->unhandledRejections_.erase(identity);
    return;
  }

  ErrorInfo error;
  if (JS_IsError(context, reason)) {
    error.name = stringProperty(context, reason, "name");
    error.message = stringProperty(context, reason, "message");
    error.stack = stringProperty(context, reason, "stack");
  }
  if (error.message.empty()) {
    error.message = toString(context, reason);
  }
  if (error.name.empty()) {
    error.name = "UnhandledPromiseRejection";
  }
  if (error.message.empty()) {
    error.message = "Unhandled promise rejection";
  }
  runtime->unhandledRejections_[identity] = std::move(error);
}

JSModuleDef* QuickJSRuntime::moduleLoader(
    JSContext* context,
    const char* moduleName,
    void* opaque) {
  auto* runtime = static_cast<QuickJSRuntime*>(opaque);
  if (runtime == nullptr || moduleName == nullptr) {
    JS_ThrowReferenceError(context, "Module loader is unavailable");
    return nullptr;
  }

  std::string source;
  {
    std::lock_guard<std::mutex> lock(runtime->moduleMutex_);
    const auto found = runtime->modules_.find(moduleName);
    if (found == runtime->modules_.end()) {
      JS_ThrowReferenceError(
          context, "Module '%s' is not available in this runtime", moduleName);
      return nullptr;
    }
    source = found->second;
  }

  JSValue compiled = JS_Eval(
      context,
      source.data(),
      source.size(),
      moduleName,
      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    return nullptr;
  }

  auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
  JS_FreeValue(context, compiled);
  return module;
}

void QuickJSRuntime::beginExecution() noexcept {
  if (executionDepth_++ == 0) {
    deadlineNs_.store(
        nowNs() + options_.executionLimitMs * 1'000'000,
        std::memory_order_relaxed);
  }
}

void QuickJSRuntime::endExecution() noexcept {
  if (executionDepth_ == 0) {
    return;
  }
  executionDepth_ -= 1;
  if (executionDepth_ == 0) {
    deadlineNs_.store(0, std::memory_order_relaxed);
    flushDeferredContextDisposals();
  }
}

bool QuickJSRuntime::deadlineExceeded() const noexcept {
  const auto deadline = deadlineNs_.load(std::memory_order_relaxed);
  return deadline > 0 && nowNs() >= deadline;
}

std::optional<ErrorInfo> QuickJSRuntime::consumeUnhandledRejection() {
  std::lock_guard<std::mutex> lock(rejectionMutex_);
  if (unhandledRejections_.empty()) {
    return std::nullopt;
  }
  auto found = unhandledRejections_.begin();
  ErrorInfo value = std::move(found->second);
  unhandledRejections_.erase(found);
  return value;
}

void QuickJSRuntime::forgetContext(QuickJSContext* context) noexcept {
  std::lock_guard<std::mutex> lock(contextsMutex_);
  contexts_.erase(
      std::remove_if(
          contexts_.begin(),
          contexts_.end(),
          [context](const std::shared_ptr<QuickJSContext>& item) {
            return item.get() == context;
          }),
      contexts_.end());
}

void QuickJSRuntime::flushDeferredContextDisposals() noexcept {
  std::vector<std::shared_ptr<QuickJSContext>> contexts;
  {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    contexts = contexts_;
  }
  for (const auto& context : contexts) {
    if (context && context->disposeRequested_) {
      context->dispose();
    }
  }
}

QuickJSContext::QuickJSContext(QuickJSRuntime& runtime)
    : runtime_(runtime), asyncState_(std::make_shared<AsyncState>()) {
  if (!runtime_.isOpen()) {
    throw std::runtime_error("QuickJS runtime is disposed");
  }
  context_ = JS_NewContext(runtime_.rawRuntime());
  if (context_ == nullptr) {
    throw std::runtime_error("Unable to allocate QuickJS context");
  }
  JS_SetContextOpaque(context_, this);

  JSValue global = JS_GetGlobalObject(context_);
  JSValue promise = JS_GetPropertyStr(context_, global, "Promise");
  JSValue prototype = JS_IsObject(promise)
      ? JS_GetPropertyStr(context_, promise, "prototype")
      : JS_UNDEFINED;
  JSValue thenFunction = JS_IsObject(prototype)
      ? JS_GetPropertyStr(context_, prototype, "then")
      : JS_UNDEFINED;
  JS_FreeValue(context_, prototype);
  JS_FreeValue(context_, promise);
  JS_FreeValue(context_, global);
  if (JS_IsException(thenFunction) || !JS_IsFunction(context_, thenFunction)) {
    if (!JS_IsException(thenFunction)) {
      JS_FreeValue(context_, thenFunction);
    } else {
      (void)takeExceptionInfo();
    }
    JS_FreeContext(context_);
    context_ = nullptr;
    throw std::runtime_error("Unable to capture QuickJS Promise.prototype.then");
  }
  promiseThen_ = thenFunction;

  installConsole();
}

QuickJSContext::~QuickJSContext() {
  dispose();
}

ExecutionResult QuickJSContext::evaluate(
    const std::string& source,
    const std::string& filename,
    EvalMode mode) {
  if (!isOpen()) {
    ExecutionResult result;
    result.reason = "destroyed";
    result.code = 1003;
    result.error.message = "QuickJS context is disposed";
    return result;
  }

  const auto started = std::chrono::steady_clock::now();
  beginExecution();
  JSValue value = JS_Eval(
      context_, source.data(), source.size(), filename.c_str(), evalFlags(mode));
  if (JS_IsException(value)) {
    ExecutionResult result = resultFromCurrentException(started);
    endExecution();
    return result;
  }

  if (mode == EvalMode::Script) {
    ExecutionResult result = resultFromValue(value, started);
    endExecution();
    return result;
  }

  // Module and async evaluation modes return a Promise. The synchronous
  // embedding API drains immediately runnable jobs but never blocks waiting
  // for an external host completion.
  endExecution();
  ExecutionResult result = awaitValue(value, started, false);
  return mode == EvalMode::AsyncScript
      ? unwrapAsyncScriptResult(std::move(result))
      : result;
}

ExecutionResult QuickJSContext::evaluateAwaited(
    const std::string& source,
    const std::string& filename,
    EvalMode mode) {
  if (!isOpen()) {
    ExecutionResult result;
    result.reason = "destroyed";
    result.code = 1003;
    result.error.message = "QuickJS context is disposed";
    return result;
  }

  const auto started = std::chrono::steady_clock::now();
  beginExecution();
  JSValue value = JS_Eval(
      context_, source.data(), source.size(), filename.c_str(), evalFlags(mode));
  if (JS_IsException(value)) {
    ExecutionResult result = resultFromCurrentException(started);
    endExecution();
    return result;
  }
  endExecution();

  ExecutionResult result = awaitValue(value, started, true);
  return mode == EvalMode::AsyncScript
      ? unwrapAsyncScriptResult(std::move(result))
      : result;
}

void QuickJSContext::registerHostFunction(
    const std::string& name,
    HostFunction function) {
  if (!isOpen()) {
    throw std::runtime_error("QuickJS context is disposed");
  }
  if (name.empty()) {
    throw std::invalid_argument("Host function name cannot be empty");
  }

  const int id = nextHostFunctionId_++;
  hostFunctions_[id] = std::move(function);

  JSValue data = JS_NewInt32(context_, id);
  JSValue functionValue = JS_NewCFunctionData(
      context_,
      &QuickJSContext::hostFunctionThunk,
      0,
      0,
      1,
      &data);
  JS_FreeValue(context_, data);
  if (JS_IsException(functionValue)) {
    const ErrorInfo error = takeExceptionInfo();
    throw std::runtime_error(
        error.message.empty() ? "Unable to create host function" : error.message);
  }

  JSValue global = JS_GetGlobalObject(context_);
  if (JS_SetPropertyStr(
          context_, global, name.c_str(), functionValue) < 0) {
    JS_FreeValue(context_, global);
    const ErrorInfo error = takeExceptionInfo();
    throw std::runtime_error(
        error.message.empty() ? "Unable to install host function" : error.message);
  }
  JS_FreeValue(context_, global);
}

void QuickJSContext::registerAsyncHostFunction(
    const std::string& name,
    AsyncHostFunction function) {
  if (!isOpen()) {
    throw std::runtime_error("QuickJS context is disposed");
  }
  if (name.empty()) {
    throw std::invalid_argument("Async host function name cannot be empty");
  }

  const int id = nextAsyncHostFunctionId_++;
  asyncHostFunctions_[id] = std::move(function);

  JSValue data = JS_NewInt32(context_, id);
  JSValue functionValue = JS_NewCFunctionData(
      context_,
      &QuickJSContext::asyncHostFunctionThunk,
      0,
      0,
      1,
      &data);
  JS_FreeValue(context_, data);
  if (JS_IsException(functionValue)) {
    const ErrorInfo error = takeExceptionInfo();
    throw std::runtime_error(
        error.message.empty()
            ? "Unable to create async host function"
            : error.message);
  }

  JSValue global = JS_GetGlobalObject(context_);
  if (JS_SetPropertyStr(
          context_, global, name.c_str(), functionValue) < 0) {
    JS_FreeValue(context_, global);
    const ErrorInfo error = takeExceptionInfo();
    throw std::runtime_error(
        error.message.empty()
            ? "Unable to install async host function"
            : error.message);
  }
  JS_FreeValue(context_, global);
}

std::uint64_t QuickJSContext::retainGlobal(const std::string& name) {
  if (!isOpen()) {
    throw std::runtime_error("QuickJS context is disposed");
  }
  JSValue global = JS_GetGlobalObject(context_);
  JSValue value = JS_GetPropertyStr(context_, global, name.c_str());
  JS_FreeValue(context_, global);
  if (JS_IsException(value)) {
    const ErrorInfo error = takeExceptionInfo();
    throw std::runtime_error(
        error.message.empty() ? "Unable to read global" : error.message);
  }
  if (!JS_IsFunction(context_, value)) {
    JS_FreeValue(context_, value);
    throw std::runtime_error("Retained global is not a function");
  }

  const std::uint64_t handle = nextHandle_++;
  retained_.emplace(handle, value);
  return handle;
}

std::uint64_t QuickJSContext::retainEvaluation(
    const std::string& source,
    const std::string& filename) {
  if (!isOpen()) {
    throw std::runtime_error("QuickJS context is disposed");
  }

  const auto started = std::chrono::steady_clock::now();
  beginExecution();
  JSValue value = JS_Eval(
      context_,
      source.data(),
      source.size(),
      filename.c_str(),
      JS_EVAL_TYPE_GLOBAL);
  endExecution();

  if (JS_IsException(value)) {
    const ErrorInfo error = takeExceptionInfo();
    throw std::runtime_error(
        error.message.empty() ? "Unable to retain evaluation" : error.message);
  }
  if (!JS_IsFunction(context_, value)) {
    JS_FreeValue(context_, value);
    throw std::runtime_error("Retained evaluation did not produce a function");
  }

  (void)started;
  const std::uint64_t handle = nextHandle_++;
  retained_.emplace(handle, value);
  return handle;
}

ExecutionResult QuickJSContext::call(
    std::uint64_t handle,
    const std::vector<Value>& args) {
  if (!isOpen()) {
    ExecutionResult result;
    result.reason = "destroyed";
    result.code = 1003;
    result.error.message = "QuickJS context is disposed";
    return result;
  }

  const auto found = retained_.find(handle);
  if (found == retained_.end()) {
    ExecutionResult result;
    result.reason = "invalid-handle";
    result.code = 1004;
    result.error.message = "Unknown retained QuickJS handle";
    return result;
  }

  std::vector<JSValue> jsArgs;
  jsArgs.reserve(args.size());
  try {
    for (const auto& arg : args) {
      jsArgs.push_back(toJSValue(arg));
      if (JS_IsException(jsArgs.back())) {
        throw std::runtime_error("Unable to convert host argument");
      }
    }
  } catch (...) {
    for (auto value : jsArgs) {
      JS_FreeValue(context_, value);
    }
    throw;
  }

  const auto started = std::chrono::steady_clock::now();
  beginExecution();
  JSValue resultValue = JS_Call(
      context_,
      found->second,
      JS_UNDEFINED,
      static_cast<int>(jsArgs.size()),
      jsArgs.data());
  for (auto value : jsArgs) {
    JS_FreeValue(context_, value);
  }

  ExecutionResult result = JS_IsException(resultValue)
      ? resultFromCurrentException(started)
      : resultFromValue(resultValue, started);
  endExecution();
  return result;
}

ExecutionResult QuickJSContext::callAwaited(
    std::uint64_t handle,
    const std::vector<Value>& args) {
  if (!isOpen()) {
    ExecutionResult result;
    result.reason = "destroyed";
    result.code = 1003;
    result.error.message = "QuickJS context is disposed";
    return result;
  }

  const auto found = retained_.find(handle);
  if (found == retained_.end()) {
    ExecutionResult result;
    result.reason = "invalid-handle";
    result.code = 1004;
    result.error.message = "Unknown retained QuickJS handle";
    return result;
  }

  std::vector<JSValue> jsArgs;
  jsArgs.reserve(args.size());
  try {
    for (const auto& arg : args) {
      jsArgs.push_back(toJSValue(arg));
      if (JS_IsException(jsArgs.back())) {
        throw std::runtime_error("Unable to convert host argument");
      }
    }
  } catch (...) {
    for (auto value : jsArgs) {
      JS_FreeValue(context_, value);
    }
    throw;
  }

  const auto started = std::chrono::steady_clock::now();
  beginExecution();
  JSValue resultValue = JS_Call(
      context_,
      found->second,
      JS_UNDEFINED,
      static_cast<int>(jsArgs.size()),
      jsArgs.data());
  for (auto value : jsArgs) {
    JS_FreeValue(context_, value);
  }

  if (JS_IsException(resultValue)) {
    ExecutionResult result = resultFromCurrentException(started);
    endExecution();
    return result;
  }
  endExecution();
  return awaitValue(resultValue, started);
}

void QuickJSContext::release(std::uint64_t handle) {
  const auto found = retained_.find(handle);
  if (found == retained_.end()) {
    return;
  }
  if (context_ != nullptr) {
    JS_FreeValue(context_, found->second);
  }
  retained_.erase(found);
}

ExecutionResult QuickJSContext::drainPendingJobsInCurrentTurn(
    std::chrono::steady_clock::time_point started,
    std::size_t maxJobs) {
  std::size_t jobs = 0;
  while (JS_IsJobPending(runtime_.rawRuntime()) && jobs < maxJobs) {
    JSContext* jobContext = nullptr;
    const int status =
        JS_ExecutePendingJob(runtime_.rawRuntime(), &jobContext);
    ++jobs;
    if (status < 0) {
      if (jobContext != nullptr && jobContext != context_) {
        JSValue exception = JS_GetException(jobContext);
        ErrorInfo error;
        if (!JS_IsUndefined(exception)) {
          error.name = stringProperty(jobContext, exception, "name");
          error.message = stringProperty(jobContext, exception, "message");
          error.stack = stringProperty(jobContext, exception, "stack");
          if (error.message.empty()) {
            error.message = toString(jobContext, exception);
          }
        }
        JS_FreeValue(jobContext, exception);
        ExecutionResult result;
        result.reason = runtime_.cancellationRequested()
            ? "cancelled"
            : runtime_.deadlineExceeded() ? "deadline" : "runtime";
        result.code = status;
        result.error = std::move(error);
        result.durationMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        result.memory = runtime_.memoryStats();
        result.outputTruncated = outputWasTruncated();
        return result;
      }
      return resultFromCurrentException(started);
    }
  }

  ExecutionResult result;
  if (jobs >= maxJobs && JS_IsJobPending(runtime_.rawRuntime())) {
    result.reason = "job-limit";
    result.code = 1005;
    result.error.message = "QuickJS pending-job limit exceeded";
  } else if (auto rejection = runtime_.consumeUnhandledRejection()) {
    result.reason = "promise-rejection";
    result.code = 1006;
    result.error = std::move(*rejection);
  }
  result.durationMs = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  result.memory = runtime_.memoryStats();
  result.outputTruncated = outputWasTruncated();
  return result;
}

ExecutionResult QuickJSContext::executePendingJobs(std::size_t maxJobs) {
  const auto started = std::chrono::steady_clock::now();
  if (!isOpen()) {
    ExecutionResult result;
    result.reason = "destroyed";
    result.code = 1003;
    result.error.message = "QuickJS context is disposed";
    return result;
  }

  processAsyncCompletions();
  beginExecution();
  ExecutionResult result = drainPendingJobsInCurrentTurn(started, maxJobs);
  endExecution();
  return result;
}

std::size_t QuickJSContext::processAsyncCompletions() {
  if (!isOpen() || !asyncState_) {
    return 0;
  }

  std::deque<QueuedAsyncCompletion> completions;
  {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    completions.swap(asyncState_->completions);
  }

  std::size_t processed = 0;
  for (auto& completion : completions) {
    const auto found = pendingPromises_.find(completion.requestId);
    if (found == pendingPromises_.end()) {
      continue;
    }

    JSValue argument = completion.result.ok
        ? toJSValue(completion.result.value)
        : errorToJSValue(completion.result.error);
    JSValue target = completion.result.ok
        ? found->second.resolve
        : found->second.reject;

    if (!JS_IsException(argument)) {
      JSValue callResult =
          JS_Call(context_, target, JS_UNDEFINED, 1, &argument);
      JS_FreeValue(context_, argument);
      if (JS_IsException(callResult)) {
        // Clear the exception here. The Promise job/rejection path reports
        // script-visible failures; a resolver invocation failure must not
        // poison the next unrelated host completion.
        (void)takeExceptionInfo();
      } else {
        JS_FreeValue(context_, callResult);
      }
    } else {
      (void)takeExceptionInfo();
    }

    JS_FreeValue(context_, found->second.resolve);
    JS_FreeValue(context_, found->second.reject);
    pendingPromises_.erase(found);
    ++processed;
  }
  return processed;
}

std::size_t QuickJSContext::pendingAsyncCount() const {
  return pendingPromises_.size();
}

void QuickJSContext::notifyAsyncActivity() noexcept {
  if (asyncState_) {
    asyncState_->activity.notify_all();
  }
}

void QuickJSContext::clearPendingAsyncPromises() noexcept {
  if (context_ != nullptr) {
    for (auto& item : pendingPromises_) {
      JS_FreeValue(context_, item.second.resolve);
      JS_FreeValue(context_, item.second.reject);
    }
  }
  pendingPromises_.clear();
  if (asyncState_) {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    asyncState_->completions.clear();
  }
}

bool QuickJSContext::markPromiseHandled(JSValueConst promise) {
  if (context_ == nullptr || JS_IsUndefined(promiseThen_)) {
    return false;
  }

  JSValue onFulfilled = JS_NewCFunction(
      context_, &QuickJSContext::promiseHandledThunk, "quickjsAwaitFulfilled", 1);
  JSValue onRejected = JS_NewCFunction(
      context_, &QuickJSContext::promiseHandledThunk, "quickjsAwaitRejected", 1);
  if (JS_IsException(onFulfilled) || JS_IsException(onRejected)) {
    if (!JS_IsException(onFulfilled)) {
      JS_FreeValue(context_, onFulfilled);
    }
    if (!JS_IsException(onRejected)) {
      JS_FreeValue(context_, onRejected);
    }
    (void)takeExceptionInfo();
    return false;
  }

  JSValue args[2] = {onFulfilled, onRejected};
  JSValue child = JS_Call(context_, promiseThen_, promise, 2, args);
  JS_FreeValue(context_, onFulfilled);
  JS_FreeValue(context_, onRejected);
  if (JS_IsException(child)) {
    (void)takeExceptionInfo();
    return false;
  }
  JS_FreeValue(context_, child);
  return true;
}

std::string QuickJSContext::getOutput(std::size_t count) const {
  std::lock_guard<std::mutex> lock(outputMutex_);
  const std::size_t take =
      count == 0 ? output_.size() : std::min(count, output_.size());
  std::string result;
  auto line = output_.begin();
  for (std::size_t index = 0; index < take; ++index, ++line) {
    if (index > 0) {
      result.push_back('\n');
    }
    result.append(*line);
  }
  return result;
}

std::string QuickJSContext::takeOutput(std::size_t count) {
  std::lock_guard<std::mutex> lock(outputMutex_);
  const std::size_t take =
      count == 0 ? output_.size() : std::min(count, output_.size());
  std::string result;
  for (std::size_t index = 0; index < take; ++index) {
    if (index > 0) {
      result.push_back('\n');
    }
    result.append(output_.front());
    outputBytes_ -= output_.front().size();
    output_.pop_front();
  }
  return result;
}

std::size_t QuickJSContext::outputCount() const {
  std::lock_guard<std::mutex> lock(outputMutex_);
  return output_.size();
}

bool QuickJSContext::outputWasTruncated() const noexcept {
  return outputTruncated_.load(std::memory_order_relaxed);
}

void QuickJSContext::dispose() noexcept {
  if (context_ == nullptr) {
    return;
  }
  if (!canDispose()) {
    disposeRequested_ = true;
    return;
  }
  disposeRequested_ = false;

  if (asyncState_) {
    asyncState_->alive.store(false, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(asyncState_->mutex);
      asyncState_->completions.clear();
    }
    asyncState_->activity.notify_all();
  }

  for (auto& item : retained_) {
    JS_FreeValue(context_, item.second);
  }
  retained_.clear();

  clearPendingAsyncPromises();

  hostFunctions_.clear();
  asyncHostFunctions_.clear();

  if (!JS_IsUndefined(promiseThen_)) {
    JS_FreeValue(context_, promiseThen_);
    promiseThen_ = JS_UNDEFINED;
  }

  JS_SetContextOpaque(context_, nullptr);
  JS_FreeContext(context_);
  context_ = nullptr;
  runtime_.forgetContext(this);
}

bool QuickJSContext::isOpen() const noexcept {
  return context_ != nullptr && runtime_.isOpen();
}

bool QuickJSContext::isExecuting() const noexcept {
  return executionDepth_ > 0;
}

bool QuickJSContext::canDispose() const noexcept {
  return !isExecuting() && !runtime_.isExecuting();
}

JSValue QuickJSContext::hostFunctionThunk(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv,
    int,
    JSValue* funcData) {
  auto* self =
      static_cast<QuickJSContext*>(JS_GetContextOpaque(context));
  if (self == nullptr) {
    return JS_ThrowInternalError(context, "QuickJS host context is unavailable");
  }

  int32_t id = 0;
  if (JS_ToInt32(context, &id, funcData[0]) < 0) {
    return JS_EXCEPTION;
  }

  const auto found = self->hostFunctions_.find(id);
  if (found == self->hostFunctions_.end()) {
    return JS_ThrowReferenceError(context, "Unknown host function");
  }

  try {
    std::vector<Value> args;
    args.reserve(static_cast<std::size_t>(std::max(argc, 0)));
    std::size_t nodes = 0;
    for (int index = 0; index < argc; ++index) {
      args.push_back(self->fromJSValue(argv[index], 0, &nodes));
    }
    const Value result = found->second(args);
    nodes = 0;
    return self->toJSValue(result, 0, &nodes);
  } catch (const std::exception& error) {
    return JS_ThrowInternalError(context, "%s", error.what());
  } catch (...) {
    return JS_ThrowInternalError(context, "Unknown host function failure");
  }
}

JSValue QuickJSContext::asyncHostFunctionThunk(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv,
    int,
    JSValue* funcData) {
  auto* self =
      static_cast<QuickJSContext*>(JS_GetContextOpaque(context));
  if (self == nullptr) {
    return JS_ThrowInternalError(
        context, "QuickJS async host context is unavailable");
  }

  int32_t id = 0;
  if (JS_ToInt32(context, &id, funcData[0]) < 0) {
    return JS_EXCEPTION;
  }

  const auto found = self->asyncHostFunctions_.find(id);
  if (found == self->asyncHostFunctions_.end()) {
    return JS_ThrowReferenceError(context, "Unknown async host function");
  }

  std::vector<Value> args;
  try {
    args.reserve(static_cast<std::size_t>(std::max(argc, 0)));
    std::size_t nodes = 0;
    for (int index = 0; index < argc; ++index) {
      args.push_back(self->fromJSValue(argv[index], 0, &nodes));
    }
  } catch (const std::exception& error) {
    return JS_ThrowTypeError(context, "%s", error.what());
  }

  JSValue resolving[2] = {JS_UNDEFINED, JS_UNDEFINED};
  JSValue promise = JS_NewPromiseCapability(context, resolving);
  if (JS_IsException(promise)) {
    return promise;
  }

  const std::uint64_t requestId = self->nextAsyncRequestId_++;
  self->pendingPromises_.emplace(
      requestId,
      PendingPromise{resolving[0], resolving[1]});

  const std::weak_ptr<AsyncState> weakState = self->asyncState_;
  auto complete = [weakState, requestId](AsyncHostResult result) {
    const auto state = weakState.lock();
    if (!state || !state->alive.load(std::memory_order_relaxed)) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->alive.load(std::memory_order_relaxed)) {
        return;
      }
      state->completions.push_back(
          QueuedAsyncCompletion{requestId, std::move(result)});
    }
    state->activity.notify_all();
  };

  try {
    found->second(args, complete);
  } catch (const std::exception& error) {
    AsyncHostResult result;
    result.ok = false;
    result.error.name = "Error";
    result.error.message = error.what();
    complete(std::move(result));
  } catch (...) {
    AsyncHostResult result;
    result.ok = false;
    result.error.name = "Error";
    result.error.message = "Unknown async host function failure";
    complete(std::move(result));
  }

  return promise;
}

JSValue QuickJSContext::consoleLogThunk(
    JSContext* context,
    JSValueConst,
    int argc,
    JSValueConst* argv) {
  auto* self =
      static_cast<QuickJSContext*>(JS_GetContextOpaque(context));
  if (self == nullptr) {
    return JS_UNDEFINED;
  }

  std::string line;
  for (int index = 0; index < argc; ++index) {
    if (index > 0) {
      line.push_back('\t');
    }
    line.append(toString(context, argv[index]));
  }
  self->appendOutput(std::move(line));
  return JS_UNDEFINED;
}

JSValue QuickJSContext::promiseHandledThunk(
    JSContext*,
    JSValueConst,
    int,
    JSValueConst*) {
  return JS_UNDEFINED;
}


ExecutionResult QuickJSContext::resultFromValue(
    JSValue value,
    std::chrono::steady_clock::time_point started,
    bool consumeValue) {
  ExecutionResult result;
  try {
    if (consumeValue) {
      result.value = fromJSValue(value);
    }
  } catch (const std::exception& error) {
    if (runtime_.cancellationRequested()) {
      result.reason = "cancelled";
      result.code = 1001;
      result.error.name = "InternalError";
      result.error.message = "Execution cancelled";
    } else if (runtime_.deadlineExceeded()) {
      result.reason = "deadline";
      result.code = 1002;
      result.error.name = "InternalError";
      result.error.message = "Execution deadline exceeded";
    } else {
      result.reason = "value-conversion";
      result.code = 1007;
      result.error.name = "ValueConversionError";
      result.error.message = error.what();
    }
    // A guest getter/Proxy trap may have left a QuickJS exception pending.
    // Consume it here so a failed conversion cannot poison the next call.
    if (context_ != nullptr) {
      JSValue pending = JS_GetException(context_);
      JS_FreeValue(context_, pending);
    }
  }

  JS_FreeValue(context_, value);
  result.durationMs = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  result.memory = runtime_.memoryStats();
  result.outputTruncated = outputWasTruncated();
  return result;
}

ExecutionResult QuickJSContext::awaitValue(
    JSValue value,
    std::chrono::steady_clock::time_point started,
    bool waitForAsyncCompletions) {
  const int initialPromiseState =
      JS_IsObject(value) ? static_cast<int>(JS_PromiseState(context_, value)) : -1;
  if (initialPromiseState < 0) {
    beginExecution();
    ExecutionResult result = resultFromValue(value, started);
    endExecution();
    return result;
  }

  beginExecution();
  const bool handled = markPromiseHandled(value);
  endExecution();
  if (!handled) {
    JS_FreeValue(context_, value);
    ExecutionResult result;
    result.reason = "runtime";
    result.code = 1;
    result.error.name = "Error";
    result.error.message = "Unable to mark awaited QuickJS Promise as handled";
    result.durationMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    result.memory = runtime_.memoryStats();
    result.outputTruncated = outputWasTruncated();
    return result;
  }

  while (isOpen() &&
         JS_PromiseState(context_, value) == JS_PROMISE_PENDING) {
    if (runtime_.cancellationRequested()) {
      JS_FreeValue(context_, value);
      clearPendingAsyncPromises();
      ExecutionResult result;
      result.reason = "cancelled";
      result.code = 1001;
      result.error.name = "InternalError";
      result.error.message = "Execution cancelled";
      result.durationMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count();
      result.memory = runtime_.memoryStats();
      result.outputTruncated = outputWasTruncated();
      return result;
    }

    processAsyncCompletions();

    if (JS_IsJobPending(runtime_.rawRuntime())) {
      // Every active JavaScript turn gets a fresh CPU budget. Time spent idle
      // in an external await (photo picker, BLE, database, etc.) is excluded.
      beginExecution();
      ExecutionResult jobs = drainPendingJobsInCurrentTurn(
          started, std::numeric_limits<std::size_t>::max());
      endExecution();
      if (!jobs.ok()) {
        JS_FreeValue(context_, value);
        clearPendingAsyncPromises();
        return jobs;
      }
      continue;
    }

    if (JS_PromiseState(context_, value) != JS_PROMISE_PENDING) {
      break;
    }

    if (!waitForAsyncCompletions) {
      JS_FreeValue(context_, value);
      clearPendingAsyncPromises();
      ExecutionResult result;
      result.reason = "pending-promise";
      result.code = 1011;
      result.error.name = "PendingPromise";
      result.error.message =
          "QuickJS evaluation returned a Promise that cannot settle synchronously";
      result.durationMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count();
      result.memory = runtime_.memoryStats();
      result.outputTruncated = outputWasTruncated();
      return result;
    }

    auto state = asyncState_;
    if (!state) {
      break;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->activity.wait_for(
        lock,
        std::chrono::milliseconds(50),
        [&] {
          return !state->completions.empty() ||
              !state->alive.load(std::memory_order_relaxed) ||
              runtime_.cancellationRequested();
        });
  }

  if (!isOpen()) {
    JS_FreeValue(context_, value);
    ExecutionResult result;
    result.reason = "destroyed";
    result.code = 1003;
    result.error.message = "QuickJS context was disposed while awaiting";
    return result;
  }

  const auto state = JS_PromiseState(context_, value);
  JSValue settled = JS_PromiseResult(context_, value);
  JS_FreeValue(context_, value);

  // Property access during conversion can execute guest getters/Proxy traps.
  // Keep the execution budget active for both fulfilled values and rejection
  // metadata so conversion cannot bypass the deadline.
  beginExecution();
  if (state == JS_PROMISE_REJECTED) {
    ExecutionResult result = resultFromPromiseRejection(settled, started);
    JS_FreeValue(context_, settled);
    endExecution();
    return result;
  }
  ExecutionResult result = resultFromValue(settled, started);
  endExecution();
  return result;
}

ExecutionResult QuickJSContext::unwrapAsyncScriptResult(
    ExecutionResult result) {
  if (!result.ok() || !result.value.has_value()) {
    return result;
  }
  const auto* object = std::get_if<Value::Object>(&result.value->data);
  if (object == nullptr) {
    return result;
  }
  const auto found = object->find("value");
  Value unwrapped =
      found == object->end() ? Value{} : found->second;
  result.value = std::move(unwrapped);
  return result;
}

ExecutionResult QuickJSContext::resultFromCurrentException(
    std::chrono::steady_clock::time_point started) {
  ExecutionResult result;
  result.error = takeExceptionInfo();
  if (runtime_.cancellationRequested()) {
    result.reason = "cancelled";
    result.code = 1001;
    result.error.message = "Execution cancelled";
  } else if (runtime_.deadlineExceeded()) {
    result.reason = "deadline";
    result.code = 1002;
    result.error.message = "Execution deadline exceeded";
  } else if (result.error.name == "SyntaxError") {
    result.reason = "syntax";
    result.code = 1009;
  } else if (
      result.error.message.find("out of memory") != std::string::npos ||
      result.error.message.find("Out of memory") != std::string::npos) {
    result.reason = "memory-limit";
    result.code = 1008;
  } else if (result.error.message.find("is not available in this runtime") != std::string::npos) {
    result.reason = "module-denied";
    result.code = 1010;
  } else {
    result.reason = "runtime";
    result.code = 1;
  }
  result.durationMs = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  result.memory = runtime_.memoryStats();
  result.outputTruncated = outputWasTruncated();
  return result;
}

ExecutionResult QuickJSContext::resultFromPromiseRejection(
    JSValueConst reason,
    std::chrono::steady_clock::time_point started) {
  ExecutionResult result;
  result.error = errorFromValue(reason);
  if (runtime_.cancellationRequested()) {
    result.reason = "cancelled";
    result.code = 1001;
    result.error.message = "Execution cancelled";
  } else if (runtime_.deadlineExceeded()) {
    result.reason = "deadline";
    result.code = 1002;
    result.error.message = "Execution deadline exceeded";
  } else {
    result.reason = "promise-rejection";
    result.code = 1006;
  }
  result.durationMs = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  result.memory = runtime_.memoryStats();
  result.outputTruncated = outputWasTruncated();
  return result;
}

ErrorInfo QuickJSContext::errorFromValue(JSValueConst value) {
  ErrorInfo result;
  if (context_ == nullptr || JS_IsUndefined(value)) {
    return result;
  }

  if (JS_IsError(context_, value)) {
    result.name = stringProperty(context_, value, "name");
    result.message = stringProperty(context_, value, "message");
    result.stack = stringProperty(context_, value, "stack");
  }
  if (result.message.empty()) {
    result.message = toString(context_, value);
  }
  if (result.name.empty()) {
    result.name = "Error";
  }
  return result;
}

JSValue QuickJSContext::errorToJSValue(const ErrorInfo& error) {
  JSValue result = JS_NewError(context_);
  if (JS_IsException(result)) {
    return result;
  }

  const auto setString = [&](const char* key, const std::string& value) {
    if (value.empty()) {
      return true;
    }
    JSValue item = JS_NewStringLen(context_, value.data(), value.size());
    if (JS_IsException(item)) {
      return false;
    }
    return JS_SetPropertyStr(context_, result, key, item) >= 0;
  };

  if (!setString("name", error.name.empty() ? "Error" : error.name) ||
      !setString("message", error.message) ||
      !setString("stack", error.stack)) {
    JS_FreeValue(context_, result);
    return JS_EXCEPTION;
  }
  return result;
}

ErrorInfo QuickJSContext::takeExceptionInfo() {
  ErrorInfo result;
  if (context_ == nullptr) {
    result.message = "QuickJS context is disposed";
    return result;
  }

  JSValue exception = JS_GetException(context_);
  result = errorFromValue(exception);
  JS_FreeValue(context_, exception);
  return result;
}

Value QuickJSContext::fromJSValue(
    JSValue value,
    int depth,
    std::size_t* nodeCount) {
  std::size_t localNodes = 0;
  if (nodeCount == nullptr) {
    nodeCount = &localNodes;
  }
  if (depth > kMaxValueDepth || ++(*nodeCount) > kMaxValueNodes) {
    throw std::runtime_error("QuickJS value exceeds host conversion limits");
  }

  if (JS_IsUndefined(value)) {
    return Value{};
  }
  if (JS_IsNull(value)) {
    return Value{nullptr};
  }
  if (JS_IsBool(value)) {
    const int result = JS_ToBool(context_, value);
    if (result < 0) {
      throw std::runtime_error("Unable to convert QuickJS boolean");
    }
    return Value{result != 0};
  }
  if (JS_IsNumber(value)) {
    double number = 0;
    if (JS_ToFloat64(context_, &number, value) < 0) {
      throw std::runtime_error("Unable to convert QuickJS number");
    }
    return Value{number};
  }
  if (JS_IsString(value)) {
    return Value{toString(context_, value)};
  }

  if (JS_IsArray(context_, value)) {
    JSValue lengthValue = JS_GetPropertyStr(context_, value, "length");
    uint32_t length = 0;
    if (JS_ToUint32(context_, &length, lengthValue) < 0) {
      JS_FreeValue(context_, lengthValue);
      throw std::runtime_error("Unable to read QuickJS array length");
    }
    JS_FreeValue(context_, lengthValue);
    if (length > kMaxValueNodes) {
      throw std::runtime_error("QuickJS array exceeds host conversion limits");
    }

    Value::Array array;
    array.reserve(length);
    for (uint32_t index = 0; index < length; ++index) {
      JSValue item = JS_GetPropertyUint32(context_, value, index);
      if (JS_IsException(item)) {
        throw std::runtime_error("Unable to read QuickJS array item");
      }
      try {
        array.push_back(fromJSValue(item, depth + 1, nodeCount));
      } catch (...) {
        JS_FreeValue(context_, item);
        throw;
      }
      JS_FreeValue(context_, item);
    }
    return Value{std::move(array)};
  }

  if (JS_IsObject(value)) {
    JSPropertyEnum* properties = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(
            context_,
            &properties,
            &count,
            value,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
      throw std::runtime_error("Unable to enumerate QuickJS object");
    }

    Value::Object object;
    try {
      for (uint32_t index = 0; index < count; ++index) {
        const char* keyText = JS_AtomToCString(context_, properties[index].atom);
        if (keyText == nullptr) {
          throw std::runtime_error("Unable to convert QuickJS object key");
        }
        std::string key(keyText);
        JS_FreeCString(context_, keyText);

        JSValue item =
            JS_GetProperty(context_, value, properties[index].atom);
        if (JS_IsException(item)) {
          throw std::runtime_error("Unable to read QuickJS object property");
        }
        try {
          object.emplace(
              std::move(key),
              fromJSValue(item, depth + 1, nodeCount));
        } catch (...) {
          JS_FreeValue(context_, item);
          throw;
        }
        JS_FreeValue(context_, item);
      }
    } catch (...) {
      JS_FreePropertyEnum(context_, properties, count);
      throw;
    }
    JS_FreePropertyEnum(context_, properties, count);
    return Value{std::move(object)};
  }

  throw std::runtime_error(
      "QuickJS host values support only undefined, null, booleans, numbers, strings, arrays, and plain objects");
}

JSValue QuickJSContext::toJSValue(
    const Value& value,
    int depth,
    std::size_t* nodeCount) {
  std::size_t localNodes = 0;
  if (nodeCount == nullptr) {
    nodeCount = &localNodes;
  }
  if (depth > kMaxValueDepth || ++(*nodeCount) > kMaxValueNodes) {
    return JS_ThrowRangeError(context_, "Host value exceeds conversion limits");
  }

  if (std::holds_alternative<std::monostate>(value.data)) {
    return JS_UNDEFINED;
  }
  if (std::holds_alternative<std::nullptr_t>(value.data)) {
    return JS_NULL;
  }
  if (const auto* boolean = std::get_if<bool>(&value.data)) {
    return JS_NewBool(context_, *boolean);
  }
  if (const auto* number = std::get_if<double>(&value.data)) {
    return JS_NewFloat64(context_, *number);
  }
  if (const auto* string = std::get_if<std::string>(&value.data)) {
    return JS_NewStringLen(context_, string->data(), string->size());
  }
  if (const auto* array = std::get_if<Value::Array>(&value.data)) {
    JSValue result = JS_NewArray(context_);
    if (JS_IsException(result)) {
      return result;
    }
    for (uint32_t index = 0; index < array->size(); ++index) {
      JSValue item = toJSValue((*array)[index], depth + 1, nodeCount);
      if (JS_IsException(item) ||
          JS_SetPropertyUint32(context_, result, index, item) < 0) {
        JS_FreeValue(context_, result);
        return JS_EXCEPTION;
      }
    }
    return result;
  }

  const auto& object = std::get<Value::Object>(value.data);
  JSValue result = JS_NewObject(context_);
  if (JS_IsException(result)) {
    return result;
  }
  for (const auto& [key, itemValue] : object) {
    JSValue item = toJSValue(itemValue, depth + 1, nodeCount);
    if (JS_IsException(item) ||
        JS_SetPropertyStr(context_, result, key.c_str(), item) < 0) {
      JS_FreeValue(context_, result);
      return JS_EXCEPTION;
    }
  }
  return result;
}

void QuickJSContext::installConsole() {
  JSValue console = JS_NewObject(context_);
  JSValue log = JS_NewCFunction(
      context_, &QuickJSContext::consoleLogThunk, "log", 1);
  JS_SetPropertyStr(context_, console, "log", log);

  JSValue global = JS_GetGlobalObject(context_);
  JS_SetPropertyStr(context_, global, "console", console);
  JS_FreeValue(context_, global);
}

void QuickJSContext::appendOutput(std::string line) {
  std::lock_guard<std::mutex> lock(outputMutex_);
  const std::size_t byteLimit = runtime_.options().maxOutputBytes;
  const std::size_t lineLimit = runtime_.options().maxOutputLines;
  if (byteLimit == 0 || lineLimit == 0 ||
      output_.size() >= lineLimit || outputBytes_ >= byteLimit) {
    outputTruncated_.store(true, std::memory_order_relaxed);
    return;
  }

  if (line.size() > byteLimit - outputBytes_) {
    line.resize(byteLimit - outputBytes_);
    outputTruncated_.store(true, std::memory_order_relaxed);
  }
  outputBytes_ += line.size();
  output_.push_back(std::move(line));
}

void QuickJSContext::beginExecution() {
  executionDepth_ += 1;
  runtime_.beginExecution();
}

void QuickJSContext::endExecution() noexcept {
  if (executionDepth_ == 0) {
    return;
  }
  executionDepth_ -= 1;
  runtime_.endExecution();
}

} // namespace rnquickjs
