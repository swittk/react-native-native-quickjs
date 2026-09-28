#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

extern "C" {
#include "../vendor/quickjs/quickjs.h"
}

namespace rnquickjs {

struct Value {
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;

  std::variant<std::monostate, std::nullptr_t, bool, double, std::string, Array, Object> data;

  Value() : data(std::monostate{}) {}
  Value(std::nullptr_t) : data(nullptr) {}
  Value(bool value) : data(value) {}
  Value(double value) : data(value) {}
  Value(int value) : data(static_cast<double>(value)) {}
  Value(std::string value) : data(std::move(value)) {}
  Value(const char* value) : data(std::string(value)) {}
  Value(Array value) : data(std::move(value)) {}
  Value(Object value) : data(std::move(value)) {}

  bool isUndefined() const noexcept { return std::holds_alternative<std::monostate>(data); }
  bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
};

struct RuntimeOptions {
  /**
   * Per-turn guest execution deadline in milliseconds.
   * 0 means unlimited execution time. Unlimited is the intentional default;
   * callers that want an automatic deadline must opt in with a positive value.
   * Explicit requestCancellation()/worker.cancel() remains active either way.
   */
  std::int64_t executionLimitMs = 0;
  std::size_t memoryLimitBytes = 32 * 1024 * 1024;
  std::size_t maxStackBytes = 2 * 1024 * 1024;
  std::size_t maxOutputBytes = 64 * 1024;
  std::size_t maxOutputLines = 1'000;
};

enum class EvalMode {
  Script,
  Module,
  AsyncScript,
  AsyncModule,
};

struct ErrorInfo {
  std::string name;
  std::string message;
  std::string stack;

  bool empty() const noexcept {
    return name.empty() && message.empty() && stack.empty();
  }
};

struct MemoryStats {
  std::size_t mallocBytes = 0;
  std::size_t memoryUsedBytes = 0;
  std::size_t mallocCount = 0;
  std::size_t objectCount = 0;
  std::size_t atomCount = 0;
};

struct ExecutionResult {
  std::string reason = "ok";
  int code = 0;
  std::optional<Value> value;
  ErrorInfo error;
  double durationMs = 0;
  MemoryStats memory;
  bool outputTruncated = false;

  bool ok() const noexcept { return reason == "ok"; }
};

class QuickJSRuntime;

class QuickJSContext final {
 public:
  using HostFunction = std::function<Value(const std::vector<Value>&)>;

  struct AsyncHostResult {
    bool ok = true;
    Value value;
    ErrorInfo error;
  };
  using AsyncHostCompletion = std::function<void(AsyncHostResult)>;
  using AsyncHostFunction = std::function<void(
      const std::vector<Value>&,
      AsyncHostCompletion)>;

  explicit QuickJSContext(QuickJSRuntime& runtime);
  ~QuickJSContext();

  QuickJSContext(const QuickJSContext&) = delete;
  QuickJSContext& operator=(const QuickJSContext&) = delete;

  ExecutionResult evaluate(
      const std::string& source,
      const std::string& filename = "<eval>",
      EvalMode mode = EvalMode::Script);

  /**
   * Evaluates on the calling/owner thread and, when the result is a Promise,
   * runs a small event loop until it settles. Host waits do not consume the
   * CPU execution deadline; each resumed JavaScript turn gets a fresh budget.
   */
  ExecutionResult evaluateAwaited(
      const std::string& source,
      const std::string& filename = "<eval>",
      EvalMode mode = EvalMode::AsyncScript);

  void registerHostFunction(const std::string& name, HostFunction function);
  void registerAsyncHostFunction(
      const std::string& name,
      AsyncHostFunction function);

  std::uint64_t retainGlobal(const std::string& name);
  std::uint64_t retainEvaluation(
      const std::string& source,
      const std::string& filename = "<retain>");
  ExecutionResult call(
      std::uint64_t handle,
      const std::vector<Value>& args = {});
  ExecutionResult callAwaited(
      std::uint64_t handle,
      const std::vector<Value>& args = {});
  void release(std::uint64_t handle);

  ExecutionResult executePendingJobs(std::size_t maxJobs = 1'000);

  /** Owner-thread pump for completions queued by arbitrary host threads. */
  std::size_t processAsyncCompletions();
  std::size_t pendingAsyncCount() const;
  void notifyAsyncActivity() noexcept;

  std::string getOutput(std::size_t count = 0) const;
  std::string takeOutput(std::size_t count = 0);
  std::size_t outputCount() const;
  bool outputWasTruncated() const noexcept;

  void dispose() noexcept;
  bool isOpen() const noexcept;
  bool isExecuting() const noexcept;
  bool canDispose() const noexcept;

  JSContext* rawContext() noexcept { return context_; }

 private:
  friend class QuickJSRuntime;

  static JSValue hostFunctionThunk(
      JSContext* ctx,
      JSValueConst thisValue,
      int argc,
      JSValueConst* argv,
      int magic,
      JSValue* funcData);
  static JSValue asyncHostFunctionThunk(
      JSContext* ctx,
      JSValueConst thisValue,
      int argc,
      JSValueConst* argv,
      int magic,
      JSValue* funcData);
  static JSValue consoleLogThunk(
      JSContext* ctx,
      JSValueConst thisValue,
      int argc,
      JSValueConst* argv);
  static JSValue promiseHandledThunk(
      JSContext* ctx,
      JSValueConst thisValue,
      int argc,
      JSValueConst* argv);

  ExecutionResult resultFromValue(
      JSValue value,
      std::chrono::steady_clock::time_point started,
      bool consumeValue = true);
  ExecutionResult awaitValue(
      JSValue value,
      std::chrono::steady_clock::time_point started,
      bool waitForAsyncCompletions = true);
  ExecutionResult unwrapAsyncScriptResult(ExecutionResult result);
  ExecutionResult resultFromCurrentException(
      std::chrono::steady_clock::time_point started);
  ExecutionResult resultFromPromiseRejection(
      JSValueConst reason,
      std::chrono::steady_clock::time_point started);
  ErrorInfo takeExceptionInfo();
  ErrorInfo errorFromValue(JSValueConst value);
  JSValue errorToJSValue(const ErrorInfo& error);
  void clearPendingAsyncPromises() noexcept;
  bool markPromiseHandled(JSValueConst promise);
  std::optional<ExecutionResult> takeAsyncCompletionFailure();
  ExecutionResult drainPendingJobsInCurrentTurn(
      std::chrono::steady_clock::time_point started,
      std::size_t maxJobs = 1'000);

  Value fromJSValue(JSValue value, int depth = 0, std::size_t* nodeCount = nullptr);
  JSValue toJSValue(const Value& value, int depth = 0, std::size_t* nodeCount = nullptr);

  class ContextPin final {
   public:
    explicit ContextPin(QuickJSContext& context) noexcept : context_(context) {
      context_.pinDepth_ += 1;
    }
    ~ContextPin() { context_.releasePin(); }

    ContextPin(const ContextPin&) = delete;
    ContextPin& operator=(const ContextPin&) = delete;

   private:
    QuickJSContext& context_;
  };

  void installConsole();
  void appendOutput(std::string line);
  void releasePin() noexcept;
  void beginExecution();
  void endExecution() noexcept;

  struct QueuedAsyncCompletion {
    std::uint64_t requestId = 0;
    AsyncHostResult result;
  };
  struct PendingPromise {
    JSValue resolve = JS_UNDEFINED;
    JSValue reject = JS_UNDEFINED;
  };
  struct AsyncState {
    std::atomic<bool> alive{true};
    mutable std::mutex mutex;
    std::condition_variable activity;
    std::deque<QueuedAsyncCompletion> completions;
  };

  QuickJSRuntime& runtime_;
  JSContext* context_ = nullptr;
  std::unordered_map<std::uint64_t, JSValue> retained_;
  std::unordered_map<int, HostFunction> hostFunctions_;
  std::unordered_map<int, AsyncHostFunction> asyncHostFunctions_;
  std::unordered_map<std::uint64_t, PendingPromise> pendingPromises_;
  std::shared_ptr<AsyncState> asyncState_;
  std::uint64_t nextHandle_ = 1;
  std::uint64_t nextAsyncRequestId_ = 1;
  int nextHostFunctionId_ = 1;
  int nextAsyncHostFunctionId_ = 1;
  std::size_t executionDepth_ = 0;
  std::size_t pinDepth_ = 0;
  bool disposeRequested_ = false;
  JSValue promiseThen_ = JS_UNDEFINED;
  std::optional<ExecutionResult> asyncCompletionFailure_;

  mutable std::mutex outputMutex_;
  std::deque<std::string> output_;
  std::size_t outputBytes_ = 0;
  std::atomic<bool> outputTruncated_{false};
};

class QuickJSRuntime final {
 public:
  explicit QuickJSRuntime(RuntimeOptions options = {});
  ~QuickJSRuntime();

  QuickJSRuntime(const QuickJSRuntime&) = delete;
  QuickJSRuntime& operator=(const QuickJSRuntime&) = delete;

  std::shared_ptr<QuickJSContext> createContext();

  void addModule(std::string name, std::string source);
  void removeModule(const std::string& name);
  void clearModules();

  void requestCancellation() noexcept;
  void resetCancellation() noexcept;
  bool cancellationRequested() const noexcept;

  void setExecutionLimitMs(std::int64_t value) noexcept;
  std::int64_t executionLimitMs() const noexcept;

  void setMemoryLimitBytes(std::size_t value) noexcept;
  std::size_t memoryLimitBytes() const noexcept;

  void setMaxStackBytes(std::size_t value) noexcept;
  std::size_t maxStackBytes() const noexcept;

  MemoryStats memoryStats() const noexcept;

  void dispose() noexcept;
  bool isOpen() const noexcept;
  bool isExecuting() const noexcept;

  JSRuntime* rawRuntime() noexcept { return runtime_; }
  const RuntimeOptions& options() const noexcept { return options_; }

 private:
  friend class QuickJSContext;

  static int interruptHandler(JSRuntime* runtime, void* opaque);
  static void promiseRejectionTracker(
      JSContext* context,
      JSValueConst promise,
      JSValueConst reason,
      int isHandled,
      void* opaque);
  static JSModuleDef* moduleLoader(
      JSContext* context,
      const char* moduleName,
      void* opaque);

  void beginExecution() noexcept;
  void endExecution() noexcept;
  bool deadlineExceeded() const noexcept;
  std::optional<ErrorInfo> consumeUnhandledRejection();
  void forgetContext(QuickJSContext* context) noexcept;
  void flushDeferredContextDisposals() noexcept;

  RuntimeOptions options_;
  JSRuntime* runtime_ = nullptr;
  std::atomic<bool> cancellationRequested_{false};
  std::atomic<std::int64_t> deadlineNs_{0};
  std::size_t executionDepth_ = 0;

  mutable std::mutex moduleMutex_;
  std::unordered_map<std::string, std::string> modules_;

  mutable std::mutex rejectionMutex_;
  std::unordered_map<const void*, ErrorInfo> unhandledRejections_;

  mutable std::mutex contextsMutex_;
  std::vector<std::shared_ptr<QuickJSContext>> contexts_;
};

} // namespace rnquickjs
