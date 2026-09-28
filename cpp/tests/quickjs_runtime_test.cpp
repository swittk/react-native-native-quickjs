#include "../QuickJSRuntime.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using rnquickjs::EvalMode;
using rnquickjs::QuickJSRuntime;
using rnquickjs::QuickJSContext;
using rnquickjs::RuntimeOptions;
using rnquickjs::Value;

namespace {
int failures = 0;

void check(bool condition, const std::string& label) {
  if (condition) {
    std::cout << "PASS: " << label << "\n";
  } else {
    ++failures;
    std::cerr << "FAIL: " << label << "\n";
  }
}

double number(const Value& value) {
  return std::get<double>(value.data);
}

const Value::Object& object(const Value& value) {
  return std::get<Value::Object>(value.data);
}
}  // namespace

int main() {
  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    auto result = context->evaluate("1 + 2", "simple.js");
    check(result.ok(), "simple eval succeeds");
    check(result.value.has_value(), "simple eval exposes value");
    check(result.value.has_value() && number(*result.value) == 3, "simple eval returns 3");

    auto bridged = context->evaluate(
        "({ hello: 'world', list: [1, true, null] })", "object.js");
    check(bridged.ok() && bridged.value.has_value(), "object bridge succeeds");
    check(
        bridged.value.has_value() &&
            std::get<std::string>(object(*bridged.value).at("hello").data) == "world",
        "object bridge preserves properties");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    auto syntax = context->evaluate("const = ;", "broken.js");
    check(!syntax.ok(), "syntax error fails");
    check(syntax.reason == "syntax", "syntax error classified");
    check(
        syntax.error.name == "SyntaxError" && !syntax.error.message.empty(),
        "syntax error has name/message");

    auto thrown = context->evaluate("throw new TypeError('boom')", "throw.js");
    check(!thrown.ok(), "runtime throw fails");
    check(
        thrown.reason == "runtime" &&
            thrown.error.name == "TypeError" &&
            thrown.error.message == "boom",
        "runtime throw is structured");
    check(
        thrown.error.stack.find("throw.js") != std::string::npos,
        "runtime stack names source");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    context->registerHostFunction(
        "nativeAdd",
        [](const std::vector<Value>& args) -> Value {
          if (args.size() != 2) {
            throw std::runtime_error("nativeAdd expects two args");
          }
          return Value{number(args[0]) + number(args[1])};
        });
    auto result = context->evaluate("nativeAdd(20, 22)", "host.js");
    check(result.ok() && result.value.has_value(), "host function succeeds");
    check(result.value.has_value() && number(*result.value) == 42, "host function returns 42");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    const auto handle = context->retainEvaluation(
        "(value) => ({ doubled: value * 2 })", "callback.js");
    auto result = context->call(handle, {Value{21}});
    check(result.ok() && result.value.has_value(), "retained callback succeeds");
    check(
        result.value.has_value() &&
            number(object(*result.value).at("doubled")) == 42,
        "retained callback returns bridged object");
    context->release(handle);
    auto released = context->call(handle);
    check(!released.ok() && released.reason == "invalid-handle", "released handle is invalid");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    std::uint64_t handle = 0;
    context->registerHostFunction(
        "releaseCurrentHandle",
        [&context, &handle](const std::vector<Value>&) -> Value {
          context->release(handle);
          return Value{true};
        });
    handle = context->retainEvaluation(
        "() => { releaseCurrentHandle(); return 42; }",
        "release-current.js");
    auto result = context->call(handle);
    check(
        result.ok() && result.value.has_value() &&
            number(*result.value) == 42,
        "retained function survives releasing its handle while executing");
    auto released = context->call(handle);
    check(
        !released.ok() && released.reason == "invalid-handle",
        "self-released retained handle is removed after the call");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    context->registerHostFunction(
        "hostObject",
        [](const std::vector<Value>&) -> Value {
          Value::Object value;
          value["__proto__"] = Value{"safe"};
          value["watched"] = Value{42};
          return Value{std::move(value)};
        });
    context->registerHostFunction(
        "hostArray",
        [](const std::vector<Value>&) -> Value {
          return Value{Value::Array{Value{7}}};
        });
    auto result = context->evaluate(
        "globalThis.setterCalls = 0;"
        "Object.defineProperty(Object.prototype, 'watched', {"
        "  configurable: true,"
        "  set() { globalThis.setterCalls++; }"
        "});"
        "Object.defineProperty(Array.prototype, '0', {"
        "  configurable: true,"
        "  set() { globalThis.setterCalls++; }"
        "});"
        "const objectValue = hostObject();"
        "const arrayValue = hostArray();"
        "({"
        "  protoOwn: Object.prototype.hasOwnProperty.call(objectValue, '__proto__'),"
        "  protoValue: objectValue['__proto__'],"
        "  watchedOwn: Object.prototype.hasOwnProperty.call(objectValue, 'watched'),"
        "  watched: objectValue.watched,"
        "  arrayOwn: Object.prototype.hasOwnProperty.call(arrayValue, '0'),"
        "  arrayValue: arrayValue[0],"
        "  setterCalls: globalThis.setterCalls"
        "})",
        "host-property-definition.js");
    check(result.ok() && result.value.has_value(), "host property definition succeeds");
    if (result.ok() && result.value.has_value()) {
      const auto& value = object(*result.value);
      check(
          std::get<bool>(value.at("protoOwn").data) &&
              std::get<std::string>(value.at("protoValue").data) == "safe",
          "host __proto__ key remains an own data property");
      check(
          std::get<bool>(value.at("watchedOwn").data) &&
              number(value.at("watched")) == 42,
          "host object property bypasses guest prototype setter");
      check(
          std::get<bool>(value.at("arrayOwn").data) &&
              number(value.at("arrayValue")) == 7,
          "host array index bypasses guest prototype setter");
      check(
          number(value.at("setterCalls")) == 0,
          "host value conversion does not invoke guest setters");
    }
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    auto setup = context->evaluate(
        "globalThis.rejectionToStringCalled = false;"
        "Promise.reject({"
        "  toString() {"
        "    globalThis.rejectionToStringCalled = true;"
        "    Promise.reject(1);"
        "    return 'nested';"
        "  }"
        "});",
        "rejection-reentrancy.js");
    check(setup.ok(), "reentrant rejection reason setup returns");
    auto called = context->evaluate(
        "globalThis.rejectionToStringCalled",
        "rejection-reentrancy-check.js");
    check(
        called.ok() && called.value.has_value() &&
            !std::get<bool>(called.value->data),
        "rejection tracker does not execute guest toString");
    auto rejection = context->executePendingJobs();
    check(
        !rejection.ok() && rejection.reason == "promise-rejection",
        "unhandled rejection is still reported after non-reentrant capture");
  }

  {
    QuickJSRuntime runtime;
    auto first = runtime.createContext();
    auto second = runtime.createContext();

    auto firstSetup = first->evaluate(
        "Promise.reject(new Error('first-context'));",
        "first-context-rejection.js");
    auto secondSetup = second->evaluate(
        "Promise.reject(new Error('second-context'));",
        "second-context-rejection.js");
    check(firstSetup.ok() && secondSetup.ok(), "cross-context rejection setup succeeds");

    auto firstResult = first->executePendingJobs();
    check(
        !firstResult.ok() &&
            firstResult.reason == "promise-rejection" &&
            firstResult.error.message == "first-context",
        "first context drains only its own rejection");

    auto secondResult = second->executePendingJobs();
    check(
        !secondResult.ok() &&
            secondResult.reason == "promise-rejection" &&
            secondResult.error.message == "second-context",
        "second context rejection remains independently reportable");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    auto setup = context->evaluate(
        "const big = 'x'.repeat(12000);"
        "for (let i = 0; i < 100; i++) {"
        "  Promise.reject(new Error(big + i));"
        "}",
        "bounded-rejections.js");
    check(setup.ok(), "bounded rejection flood setup succeeds");

    auto rejection = context->executePendingJobs();
    check(
        !rejection.ok() && rejection.reason == "promise-rejection",
        "bounded rejection flood reports one rejection");
    check(
        rejection.error.name.size() <= 4096 &&
            rejection.error.message.size() <= 4096 &&
            rejection.error.stack.size() <= 4096,
        "host-side rejection fields stay bounded");

    auto drained = context->executePendingJobs();
    check(
        drained.ok(),
        "draining one rejection clears stale records for that context");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    auto setup = context->evaluate(
        "globalThis.jobValue = 0;"
        "Promise.resolve(7).then(v => { globalThis.jobValue = v * 6; });",
        "promise.js");
    check(setup.ok(), "promise setup succeeds");
    auto jobs = context->executePendingJobs();
    check(jobs.ok(), "pending jobs drain");
    auto value = context->evaluate("jobValue", "promise-check.js");
    check(
        value.ok() && value.value.has_value() && number(*value.value) == 42,
        "promise microtask executed");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();

    auto syncAsync = context->evaluate(
        "await 1; 42",
        "sync-async.js",
        EvalMode::AsyncScript);
    check(
        syncAsync.ok() && syncAsync.value.has_value() &&
            number(*syncAsync.value) == 42,
        "synchronous async-script evaluation settles and unwraps completion");

    auto awaitedAsync = context->evaluateAwaited(
        "await 1; 42",
        "awaited-async.js",
        EvalMode::AsyncScript);
    check(
        awaitedAsync.ok() && awaitedAsync.value.has_value() &&
            number(*awaitedAsync.value) == 42,
        "awaited async-script evaluation unwraps completion");

    auto manyJobs = context->evaluateAwaited(
        "let n = 0; for (let i = 0; i < 2000; i++) { await 0; n++; } n;",
        "many-jobs.js",
        EvalMode::AsyncScript);
    check(
        manyJobs.ok() && manyJobs.value.has_value() &&
            number(*manyJobs.value) == 2000,
        "awaited evaluation drains more than 1000 legitimate microtasks");

    context->registerAsyncHostFunction(
        "neverSync",
        [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion) {});
    auto pending = context->evaluate(
        "await neverSync(); 42",
        "pending-sync.js",
        EvalMode::AsyncScript);
    check(
        !pending.ok() && pending.reason == "pending-promise",
        "synchronous async evaluation reports an unresolved host Promise");
    check(
        context->pendingAsyncCount() == 0,
        "unresolved synchronous Promise releases host resolver handles");

    auto rejected = context->evaluateAwaited(
        "await 0; throw new TypeError('x');",
        "awaited-rejection.js",
        EvalMode::AsyncScript);
    check(
        !rejected.ok() && rejected.reason == "promise-rejection" &&
            rejected.error.name == "TypeError" &&
            rejected.error.message == "x",
        "awaited rejection preserves structured TypeError metadata");

    auto immediateRejected = context->evaluateAwaited(
        "throw new Error('stale');",
        "immediate-rejection.js",
        EvalMode::AsyncScript);
    check(
        !immediateRejected.ok() &&
            immediateRejected.reason == "promise-rejection" &&
            immediateRejected.error.message == "stale",
        "immediate async rejection is consumed by the awaited result");

    auto afterRejected = context->evaluateAwaited(
        "await 0; 1",
        "after-rejection.js",
        EvalMode::AsyncScript);
    check(
        afterRejected.ok() && afterRejected.value.has_value() &&
            number(*afterRejected.value) == 1,
        "handled top-level rejection does not leak into later evaluation");
  }

  {
    RuntimeOptions options;
    options.executionLimitMs = 15;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();
    auto conversion = context->evaluateAwaited(
        "await 0; ({ get x() { for (;;) {} } })",
        "conversion-deadline.js",
        EvalMode::AsyncScript);
    check(
        !conversion.ok() && conversion.reason == "deadline",
        "result conversion remains inside the execution deadline");
  }

  {
    RuntimeOptions options;
    options.executionLimitMs = 20;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();
    context->registerHostFunction(
        "nestedEval",
        [&context](const std::vector<Value>&) -> Value {
          auto nested = context->evaluate("21 * 2", "nested-inner.js");
          if (!nested.ok() || !nested.value.has_value() ||
              number(*nested.value) != 42) {
            throw std::runtime_error("nested evaluation failed");
          }
          return Value{true};
        });

    std::thread watchdog([&runtime] {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      runtime.requestCancellation();
    });
    const auto started = std::chrono::steady_clock::now();
    auto result = context->evaluate(
        "nestedEval(); for (;;) {}",
        "nested-deadline.js");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    watchdog.join();
    check(
        result.reason == "deadline",
        "nested evaluation preserves the outer execution deadline");
    check(
        elapsed.count() < 200,
        "nested evaluation does not refresh or clear the outer deadline");
  }

  {
    RuntimeOptions options;
    options.executionLimitMs = 20;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();

    auto setup = context->evaluate(
        "Object.defineProperty(globalThis, 'slowGlobal', {"
        "get() { for (;;) {} }"
        "});",
        "retain-global-setup.js");
    check(setup.ok(), "retainGlobal deadline setup succeeds");

    const auto started = std::chrono::steady_clock::now();
    bool threw = false;
    try {
      (void)context->retainGlobal("slowGlobal");
    } catch (const std::exception&) {
      threw = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    check(threw, "retainGlobal interrupts guest getter");
    check(
        elapsed.count() < 200,
        "retainGlobal guest getter remains inside execution deadline");
  }

  {
    RuntimeOptions options;
    options.executionLimitMs = 20;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();

    const auto started = std::chrono::steady_clock::now();
    bool threw = false;
    try {
      (void)context->retainEvaluation(
          "throw { toString() { for (;;) {} } }",
          "retain-error-stringify.js");
    } catch (const std::exception&) {
      threw = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    check(threw, "retainEvaluation reports thrown guest value");
    check(
        elapsed.count() < 200,
        "retainEvaluation exception stringification remains deadline-bounded");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    context->registerHostFunction(
        "attemptDispose",
        [&runtime, &context](const std::vector<Value>&) -> Value {
          context->dispose();
          runtime.dispose();
          return Value{context->isOpen() && runtime.isOpen()};
        });
    auto result = context->evaluate(
        "attemptDispose(); 42",
        "dispose-reentrant.js");
    check(
        result.ok() && result.value.has_value() &&
            number(*result.value) == 42,
        "native disposal is deferred while QuickJS is executing");
    check(
        !context->isOpen() && runtime.isOpen(),
        "deferred context disposal completes after the outer execution turn");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    context->registerHostFunction(
        "attemptAsyncDispose",
        [&runtime, &context](const std::vector<Value>&) -> Value {
          context->dispose();
          runtime.dispose();
          return Value{true};
        });
    auto result = context->evaluateAwaited(
        "attemptAsyncDispose(); await 0; 42",
        "dispose-reentrant-async.js",
        EvalMode::AsyncScript);
    check(
        result.ok() && result.value.has_value() &&
            number(*result.value) == 42,
        "async reentrant disposal keeps context pinned through await");
    check(
        !context->isOpen() && runtime.isOpen(),
        "async deferred disposal runs only after awaited value is released");
  }

  {
    QuickJSRuntime runtime;
    auto first = runtime.createContext();
    auto second = runtime.createContext();
    second->registerHostFunction(
        "disposeOtherContext",
        [&first](const std::vector<Value>&) -> Value {
          first->dispose();
          return Value{first->isOpen()};
        });
    auto result = second->evaluate(
        "disposeOtherContext(); 42",
        "cross-context-dispose.js");
    check(
        result.ok() && result.value.has_value() &&
            number(*result.value) == 42,
        "disposing another context cannot disrupt active runtime execution");
    check(
        !first->isOpen(),
        "sibling context disposal is deferred until runtime execution is idle");
  }

  {
    QuickJSRuntime runtime;
    auto first = runtime.createContext();
    std::weak_ptr<QuickJSContext> firstWeak = first;
    auto second = runtime.createContext();
    second->registerHostFunction(
        "releaseSiblingWrapper",
        [&first](const std::vector<Value>&) -> Value {
          first->dispose();
          first.reset();
          return Value{true};
        });
    auto result = second->evaluate(
        "releaseSiblingWrapper(); 42",
        "deferred-context-destruction.js");
    check(
        result.ok() && result.value.has_value() &&
            number(*result.value) == 42,
        "sibling wrapper release cannot corrupt active runtime execution");
    check(
        firstWeak.expired(),
        "deferred context disposal completes after outer execution ends");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    context->registerAsyncHostFunction(
        "hostDelay",
        [](const std::vector<Value>& args, QuickJSContext::AsyncHostCompletion complete) {
          const int value = static_cast<int>(number(args.at(0)));
          const int delayMs = static_cast<int>(number(args.at(1)));
          std::thread([value, delayMs, complete = std::move(complete)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            QuickJSContext::AsyncHostResult result;
            result.value = Value{value};
            complete(std::move(result));
          }).detach();
        });

    auto awaited = context->evaluateAwaited(
        "globalThis.asyncValues = await Promise.all(["
        "hostDelay(1, 30), hostDelay(2, 5)]);",
        "async-host.js",
        EvalMode::AsyncScript);
    check(awaited.ok(), "async host Promises settle automatically");

    auto values = context->evaluate("globalThis.asyncValues", "async-values.js");
    bool valuesOk = values.ok() && values.value.has_value();
    if (valuesOk) {
      const auto& array = std::get<Value::Array>(values.value->data);
      valuesOk =
          array.size() == 2 && number(array[0]) == 1 && number(array[1]) == 2;
    }
    check(valuesOk, "concurrent async host Promises preserve Promise.all ordering");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    context->registerAsyncHostFunction(
        "hostFail",
        [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion complete) {
          QuickJSContext::AsyncHostResult result;
          result.ok = false;
          result.error.name = "HostError";
          result.error.message = "camera denied";
          complete(std::move(result));
        });

    auto awaited = context->evaluateAwaited(
        "try { await hostFail(); } catch (error) { "
        "globalThis.hostError = error.name + ':' + error.message; }",
        "async-reject.js",
        EvalMode::AsyncScript);
    check(awaited.ok(), "async host rejection can be caught with normal await/try");

    auto error = context->evaluate("globalThis.hostError", "host-error.js");
    check(
        error.ok() && error.value.has_value() &&
            std::get<std::string>(error.value->data) ==
                "HostError:camera denied",
        "async host rejection preserves error name/message");
  }

  {
    RuntimeOptions options;
    options.executionLimitMs = 20;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();
    context->registerAsyncHostFunction(
        "hostObjectAsync",
        [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion complete) {
          QuickJSContext::AsyncHostResult result;
          Value::Object value;
          value["answer"] = Value{42};
          result.value = Value{std::move(value)};
          complete(std::move(result));
        });
    auto result = context->evaluateAwaited(
        "Object.defineProperty(Object.prototype, 'then', {"
        "  configurable: true,"
        "  get() { for (;;) {} }"
        "});"
        "await hostObjectAsync();",
        "completion-then-deadline.js",
        EvalMode::AsyncScript);
    check(
        !result.ok() && result.reason == "deadline",
        "configured deadline covers async Promise resolution then-getter");
  }

  {
    rnquickjs::ExecutionResult result;
    std::atomic<std::size_t> pendingAfterCancel{999};
    std::atomic<QuickJSRuntime*> active{nullptr};
    std::mutex readyMutex;
    std::condition_variable readyCondition;
    bool ready = false;
    const auto started = std::chrono::steady_clock::now();

    std::thread worker([&] {
      RuntimeOptions options;
      options.executionLimitMs = 10'000;
      QuickJSRuntime runtime(options);
      auto context = runtime.createContext();
      context->registerAsyncHostFunction(
          "never",
          [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion) {
            // Deliberately never settles. Cancellation must wake the event loop.
          });
      active.store(&runtime, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
      }
      readyCondition.notify_one();
      result = context->evaluateAwaited(
          "await never();", "cancel-await.js", EvalMode::AsyncScript);
      pendingAfterCancel.store(
          context->pendingAsyncCount(), std::memory_order_release);
      active.store(nullptr, std::memory_order_release);
    });

    {
      std::unique_lock<std::mutex> lock(readyMutex);
      readyCondition.wait(lock, [&] { return ready; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (auto* runtime = active.load(std::memory_order_acquire)) {
      runtime->requestCancellation();
    }
    worker.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    check(
        result.reason == "cancelled",
        "cross-thread cancellation wakes an awaited host Promise");
    check(
        elapsed.count() < 500,
        "awaited host Promise cancellation remains prompt");
    check(
        pendingAfterCancel.load(std::memory_order_acquire) == 0,
        "cancelled host Promise releases pending resolver handles");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    context->registerAsyncHostFunction(
        "hostDouble",
        [](const std::vector<Value>& args, QuickJSContext::AsyncHostCompletion complete) {
          QuickJSContext::AsyncHostResult result;
          result.value = Value{number(args.at(0)) * 2};
          complete(std::move(result));
        });
    const auto handle = context->retainEvaluation(
        "async value => await hostDouble(value)", "retained-async.js");
    auto result = context->callAwaited(handle, {Value{21}});
    check(
        result.ok() && result.value.has_value() && number(*result.value) == 42,
        "retained callback awaits async host Promise");
    context->release(handle);
  }

  {
    rnquickjs::ExecutionResult result;
    std::atomic<QuickJSRuntime*> active{nullptr};
    std::mutex readyMutex;
    std::condition_variable readyCondition;
    bool ready = false;

    std::thread worker([&] {
      QuickJSRuntime runtime;
      auto context = runtime.createContext();
      active.store(&runtime, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
      }
      readyCondition.notify_one();
      result = context->evaluateAwaited(
          "await new Promise(() => {})",
          "intentional-pending.js",
          EvalMode::AsyncScript);
      active.store(nullptr, std::memory_order_release);
    });

    {
      std::unique_lock<std::mutex> lock(readyMutex);
      readyCondition.wait(lock, [&] { return ready; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (auto* runtime = active.load(std::memory_order_acquire)) {
      runtime->requestCancellation();
    }
    worker.join();
    check(
        result.reason == "cancelled",
        "never-settling awaited Promise intentionally remains pending until cancellation");
  }

  {
    rnquickjs::ExecutionResult result;
    std::atomic<QuickJSRuntime*> active{nullptr};
    std::mutex readyMutex;
    std::condition_variable readyCondition;
    bool ready = false;

    std::thread worker([&] {
      QuickJSRuntime runtime;
      check(
          runtime.executionLimitMs() == 0,
          "default execution deadline is disabled");
      auto context = runtime.createContext();
      active.store(&runtime, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready = true;
      }
      readyCondition.notify_one();
      result = context->evaluate("for (;;) {}", "unlimited-cancel.js");
      active.store(nullptr, std::memory_order_release);
    });

    {
      std::unique_lock<std::mutex> lock(readyMutex);
      readyCondition.wait(lock, [&] { return ready; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (auto* runtime = active.load(std::memory_order_acquire)) {
      runtime->requestCancellation();
    }
    worker.join();
    check(
        result.reason == "cancelled",
        "default-unlimited execution remains externally cancellable");
  }

  {
    RuntimeOptions options;
    options.executionLimitMs = 15;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();

    std::thread watchdog([&runtime] {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      runtime.requestCancellation();
    });
    auto result = context->evaluateAwaited(
        "await 0; for (;;) {}",
        "resumed-deadline.js",
        EvalMode::AsyncScript);
    watchdog.join();
    check(
        !result.ok() && result.reason == "deadline",
        "resumed async job preserves configured deadline classification");
  }

  {
    RuntimeOptions options;
    options.executionLimitMs = 15;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();
    auto result = context->evaluate("for (;;) {}", "deadline.js");
    check(!result.ok() && result.reason == "deadline", "deadline interrupts infinite loop");
    runtime.setExecutionLimitMs(0);
    check(
        runtime.executionLimitMs() == 0,
        "setExecutionLimitMs(0) disables the automatic deadline");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    runtime.requestCancellation();
    auto result = context->evaluate("for (;;) {}", "cancel.js");
    check(!result.ok() && result.reason == "cancelled", "explicit cancellation interrupts");
    runtime.resetCancellation();
    auto recovered = context->evaluate("6 * 7", "recovered.js");
    check(
        recovered.ok() && recovered.value.has_value() && number(*recovered.value) == 42,
        "runtime reusable after cancellation reset");
  }

  {
    RuntimeOptions options;
    options.memoryLimitBytes = 2 * 1024 * 1024;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();
    auto result = context->evaluate(
        "const a = []; for (let i = 0; i < 500000; i++) "
        "a.push('xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'); a.length;",
        "memory.js");
    check(!result.ok(), "memory pressure is bounded");
    check(
        result.reason == "memory-limit" ||
            result.error.message.find("memory") != std::string::npos,
        "memory limit failure is reported");
  }

  {
    QuickJSRuntime runtime;
    runtime.addModule("math", "export const answer = 42;");
    auto context = runtime.createContext();
    auto allowed = context->evaluate(
        "import { answer } from 'math'; globalThis.moduleAnswer = answer;",
        "entry.mjs",
        EvalMode::Module);
    check(allowed.ok(), "explicit in-memory module imports");
    auto value = context->evaluate("moduleAnswer", "module-check.js");
    check(
        value.ok() && value.value.has_value() && number(*value.value) == 42,
        "module side effect visible");

    auto denied = context->evaluate(
        "import value from 'not-granted'; globalThis.nope = value;",
        "denied.mjs",
        EvalMode::Module);
    check(!denied.ok() && denied.reason == "module-denied", "unknown module denied");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    auto logged = context->evaluate("console.log('hello', 42); 'ok';", "console.js");
    check(logged.ok(), "console script succeeds");
    check(context->getOutput() == "hello\t42", "output read is non-destructive");
    check(context->takeOutput() == "hello\t42", "console output captured");
  }

  {
    auto runtime = std::make_unique<QuickJSRuntime>();
    auto context = runtime->createContext();
    auto value = context->evaluate("21 * 2", "lifetime.js");
    check(value.ok(), "context works before runtime disposal");
    runtime->dispose();
    check(!context->isOpen(), "runtime disposal closes child contexts safely");
  }

  if (failures != 0) {
    std::cerr << failures << " native QuickJS test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All native QuickJS tests passed\n";
  return EXIT_SUCCESS;
}
