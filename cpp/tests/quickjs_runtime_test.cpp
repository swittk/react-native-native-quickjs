#include "../QuickJSRuntime.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

using rnquickjs::EvalMode;
using rnquickjs::QuickJSRuntime;
using rnquickjs::QuickJSExecutionException;
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

#if defined(__linux__)
struct SmallStackResult {
  bool runtimeCreated = false;
  bool stackWasClamped = false;
  bool recursionWasCaught = false;
};

void* runQuickJSOnSmallNativeStack(void* opaque) {
  auto* result = static_cast<SmallStackResult*>(opaque);
  try {
    RuntimeOptions options;
    options.maxStackBytes = 32 * 1024 * 1024;
    QuickJSRuntime runtime(options);
    result->runtimeCreated = true;
    result->stackWasClamped = runtime.maxStackBytes() < 512 * 1024;

    auto context = runtime.createContext();
    auto recursion = context->evaluate(
        "function recurse() { return 1 + recurse(); } recurse();",
        "small-native-stack-recursion.js");
    result->recursionWasCaught =
        !recursion.ok() &&
        recursion.error.message.find("stack") != std::string::npos;
  } catch (...) {
    result->recursionWasCaught = false;
  }
  return nullptr;
}
#endif
}  // namespace

int main() {
  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    auto result = context->evaluate("1 + 2", "simple.js");
    check(result.ok(), "simple eval succeeds");
    check(result.value.has_value(), "simple eval exposes value");
    check(result.value.has_value() && number(*result.value) == 3, "simple eval returns 3");
    check(
        result.memory.memoryUsedBytes == 0 &&
            result.memory.mallocBytes == 0 &&
            result.memory.objectCount == 0,
        "per-result memory stats are disabled by default");
    check(
        runtime.memoryStats().memoryUsedBytes > 0,
        "on-demand runtime memory stats remain available");

    auto bridged = context->evaluate(
        "({ hello: 'world', list: [1, true, null] })", "object.js");
    check(bridged.ok() && bridged.value.has_value(), "object bridge succeeds");
    check(
        bridged.value.has_value() &&
            std::get<std::string>(object(*bridged.value).at("hello").data) == "world",
        "object bridge preserves properties");
  }

  {
    RuntimeOptions options;
    options.collectResultMemoryStats = true;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();
    auto result = context->evaluate("21 * 2", "memory-stats-opt-in.js");
    check(
        result.ok() && result.memory.memoryUsedBytes > 0,
        "per-result memory stats are available when explicitly enabled");

    auto queuedJob = context->evaluate(
        "Promise.resolve().then(() => 1); void 0;",
        "memory-stats-pending-jobs.js");
    check(queuedJob.ok(), "memory-stats pending-job setup succeeds");
    auto drained = context->executePendingJobs();
    check(
        drained.ok() && drained.memory.memoryUsedBytes > 0,
        "public executePendingJobs preserves opted-in memory stats");
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

    auto hostileMetadata = context->evaluate(
        "const error = new Error('original failure');"
        "Object.defineProperty(error, 'name', {"
        "  get() { throw new Error('metadata getter failure'); }"
        "});"
        "throw error;",
        "hostile-error-metadata.js");
    check(
        !hostileMetadata.ok() &&
            hostileMetadata.error.message == "original failure",
        "error extraction tolerates throwing metadata getters");
    check(
        !JS_HasException(context->rawContext()),
        "error metadata extraction does not leave a pending exception");

    auto hostileStringify = context->evaluate(
        "throw { toString() { throw new Error('nested stringify failure'); } };",
        "hostile-error-stringify.js");
    check(
        !hostileStringify.ok() &&
            !JS_HasException(context->rawContext()),
        "failed exception stringification does not poison the next call");

    auto afterHostileError = context->evaluate("21 * 2", "after-hostile-error.js");
    check(
        afterHostileError.ok() && afterHostileError.value.has_value() &&
            number(*afterHostileError.value) == 42,
        "context remains reusable after hostile error metadata");

    auto oversizedError = context->evaluate(
        "throw new Error('x'.repeat(200000));",
        "oversized-error.js");
    check(
        !oversizedError.ok() &&
            oversizedError.error.name.size() <= 64 * 1024 &&
            oversizedError.error.message.size() <= 64 * 1024 &&
            oversizedError.error.stack.size() <= 64 * 1024,
        "ordinary error metadata stays host-side bounded");

    auto oversizedThrownString = context->evaluate(
        "throw 'y'.repeat(200000);",
        "oversized-thrown-string.js");
    check(
        !oversizedThrownString.ok() &&
            oversizedThrownString.error.message.size() <= 64 * 1024,
        "non-Error thrown stringification stays host-side bounded");
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
    RuntimeOptions options;
    options.executionLimitMs = 20;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();

    auto setup = context->evaluate(
        "globalThis.registrationSetterCalls = 0;"
        "Object.defineProperty(globalThis, 'lateSyncHost', {"
        "  configurable: true,"
        "  set() { globalThis.registrationSetterCalls++; }"
        "});"
        "Object.defineProperty(globalThis, 'lateAsyncHost', {"
        "  configurable: true,"
        "  set() { globalThis.registrationSetterCalls++; }"
        "});"
        "void 0;",
        "registration-setter-setup.js");
    check(setup.ok(), "host registration setter setup succeeds");

    context->registerHostFunction(
        "lateSyncHost",
        [](const std::vector<Value>&) -> Value { return Value{42}; });
    context->registerAsyncHostFunction(
        "lateAsyncHost",
        [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion complete) {
          QuickJSContext::AsyncHostResult result;
          result.value = Value{7};
          complete(std::move(result));
        });

    auto registered = context->evaluateAwaited(
        "({"
        "  setterCalls: globalThis.registrationSetterCalls,"
        "  syncValue: lateSyncHost(),"
        "  asyncValue: await lateAsyncHost()"
        "})",
        "registration-setter-check.js",
        EvalMode::AsyncScript);
    check(
        registered.ok() && registered.value.has_value(),
        "host registration bypasses guest global setters");
    if (registered.ok() && registered.value.has_value()) {
      const auto& value = object(*registered.value);
      check(
          number(value.at("setterCalls")) == 0,
          "sync and async host registration do not invoke guest setters");
      check(
          number(value.at("syncValue")) == 42 &&
              number(value.at("asyncValue")) == 7,
          "registered host functions remain callable after safe definition");
    }
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
        "plainOnly",
        [](const std::vector<Value>& args) -> Value {
          return Value{static_cast<int>(args.size())};
        });

    auto plain = context->evaluate(
        "plainOnly({ value: 1 }) + plainOnly(Object.assign("
        "Object.create(null), { value: 2 }))",
        "plain-object-arguments.js");
    check(
        plain.ok() && plain.value.has_value() && number(*plain.value) == 2,
        "plain and null-prototype objects cross the host bridge");

    auto date = context->evaluate(
        "try { plainOnly(new Date()); 'accepted'; }"
        "catch (error) { error.name; }",
        "non-plain-date.js");
    check(
        date.ok() && date.value.has_value() &&
            std::get<std::string>(date.value->data) == "TypeError",
        "Date instances are rejected as non-plain host arguments");

    auto function = context->evaluate(
        "try { plainOnly(function nope() {}); 'accepted'; }"
        "catch (error) { error.name; }",
        "non-plain-function.js");
    check(
        function.ok() && function.value.has_value() &&
            std::get<std::string>(function.value->data) == "TypeError",
        "functions are rejected as non-plain host arguments");

    auto proxy = context->evaluate(
        "globalThis.bridgeProxyTrapCalls = 0;"
        "const proxy = new Proxy({}, {"
        "  getPrototypeOf() { bridgeProxyTrapCalls++; return Object.prototype; }"
        "});"
        "let proxyError = '';"
        "try { plainOnly(proxy); } catch (error) { proxyError = error.name; }"
        "({ proxyError, trapCalls: bridgeProxyTrapCalls });",
        "non-plain-proxy.js");
    check(
        proxy.ok() && proxy.value.has_value() &&
            std::get<std::string>(
                object(*proxy.value).at("proxyError").data) == "TypeError" &&
            number(object(*proxy.value).at("trapCalls")) == 0,
        "Proxy arguments are rejected without running getPrototypeOf traps");

    auto arrayProxy = context->evaluate(
        "globalThis.bridgeArrayProxyGetCalls = 0;"
        "const arrayProxy = new Proxy([1, 2], {"
        "  get(target, key, receiver) {"
        "    bridgeArrayProxyGetCalls++;"
        "    return Reflect.get(target, key, receiver);"
        "  }"
        "});"
        "let arrayProxyError = '';"
        "try { plainOnly(arrayProxy); }"
        "catch (error) { arrayProxyError = error.name; }"
        "({ arrayProxyError, getCalls: bridgeArrayProxyGetCalls });",
        "non-plain-array-proxy.js");
    check(
        arrayProxy.ok() && arrayProxy.value.has_value() &&
            std::get<std::string>(
                object(*arrayProxy.value).at("arrayProxyError").data) ==
                "TypeError" &&
            number(object(*arrayProxy.value).at("getCalls")) == 0,
        "Proxy-wrapped arrays are rejected without running guest traps");

    auto dateResult = context->evaluate("new Date()", "non-plain-date-result.js");
    check(
        !dateResult.ok() && dateResult.reason == "value-conversion",
        "Date results are rejected instead of silently becoming plain objects");

    auto functionResult =
        context->evaluate("(() => 42)", "non-plain-function-result.js");
    check(
        !functionResult.ok() && functionResult.reason == "value-conversion",
        "function results are rejected instead of silently becoming plain objects");
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
    context->registerHostFunction(
        "echoObjectKeys",
        [](const std::vector<Value>& args) -> Value {
          return args.empty() ? Value{} : args[0];
        });
    auto result = context->evaluate(
        "const input = {};"
        "input['a\\u0000b'] = 1;"
        "input['a'] = 2;"
        "const output = echoObjectKeys(input);"
        "({ keys: Object.keys(output), first: output['a\\u0000b'], second: output.a });",
        "embedded-nul-keys.js");
    bool preserved = false;
    if (result.ok() && result.value.has_value()) {
      const auto& resultObject = object(*result.value);
      const auto foundKeys = resultObject.find("keys");
      if (foundKeys != resultObject.end() &&
          std::holds_alternative<Value::Array>(foundKeys->second.data)) {
        const auto& keys = std::get<Value::Array>(foundKeys->second.data);
        bool foundEmbeddedNul = false;
        bool foundPlainA = false;
        for (const auto& keyValue : keys) {
          if (!std::holds_alternative<std::string>(keyValue.data)) {
            continue;
          }
          const auto& key = std::get<std::string>(keyValue.data);
          foundEmbeddedNul =
              foundEmbeddedNul || key == std::string("a\0b", 3);
          foundPlainA = foundPlainA || key == "a";
        }
        preserved =
            keys.size() == 2 && foundEmbeddedNul && foundPlainA &&
            number(resultObject.at("first")) == 1 &&
            number(resultObject.at("second")) == 2;
      }
    }
    check(
        preserved,
        "embedded-NUL object keys remain distinct across the host bridge");
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
        "});"
        "void 0;",
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
        "Promise.reject(new Error('first-context')); void 0;",
        "first-context-rejection.js");
    auto secondSetup = second->evaluate(
        "Promise.reject(new Error('second-context')); void 0;",
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
        "}"
        "void 0;",
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
        "Promise.resolve(7).then(v => { globalThis.jobValue = v * 6; });"
        "void 0;",
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

    Value tooDeep{1};
    for (int depth = 0; depth < 40; ++depth) {
      tooDeep = Value{Value::Array{std::move(tooDeep)}};
    }
    context->registerAsyncHostFunction(
        "tooDeepCompletion",
        [tooDeep = std::move(tooDeep)](
            const std::vector<Value>&,
            QuickJSContext::AsyncHostCompletion complete) {
          QuickJSContext::AsyncHostResult result;
          result.value = tooDeep;
          complete(std::move(result));
        });
    auto failedCompletion = context->evaluateAwaited(
        "await tooDeepCompletion(); 42",
        "async-completion-conversion.js",
        EvalMode::AsyncScript);
    check(
        !failedCompletion.ok() &&
            failedCompletion.reason == "value-conversion",
        "async completion conversion failure is surfaced");
    check(
        context->pendingAsyncCount() == 0,
        "failed async completion does not orphan a pending Promise");

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

    QuickJSContext::AsyncHostCompletion completeAfterPending;
    context->registerAsyncHostFunction(
        "lateAfterPending",
        [&completeAfterPending](
            const std::vector<Value>&,
            QuickJSContext::AsyncHostCompletion complete) {
          completeAfterPending = std::move(complete);
        });
    auto stalePending = context->evaluate(
        "await lateAfterPending(); 42",
        "late-after-pending.js",
        EvalMode::AsyncScript);
    check(
        !stalePending.ok() && stalePending.reason == "pending-promise" &&
            static_cast<bool>(completeAfterPending),
        "late-completion regression setup returns pending-promise");
    QuickJSContext::AsyncHostResult staleCompletion;
    staleCompletion.value = Value{42};
    completeAfterPending(std::move(staleCompletion));
    check(
        context->queuedAsyncCompletionCount() == 0,
        "late completion after pending-promise cleanup is dropped");

    context->registerAsyncHostFunction(
        "neverCompleteFlood",
        [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion) {});
    auto boundedAsyncFlood = context->evaluate(
        "let overflowName = '';"
        "for (let i = 0; i < 2000; i++) {"
        "  try { neverCompleteFlood(i); }"
        "  catch (error) { overflowName = error.name; break; }"
        "}"
        "overflowName;",
        "bounded-async-host-flood.js");
    check(
        boundedAsyncFlood.ok() && boundedAsyncFlood.value.has_value() &&
            std::get<std::string>(boundedAsyncFlood.value->data) ==
                "RangeError",
        "async host call flood is bounded with a catchable guest error");
    check(
        context->pendingAsyncCount() == 1024,
        "async host pending resolver count stays within its hard cap");

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

    auto strayRejected = context->evaluateAwaited(
        "Promise.reject(new Error('same-call')); 5",
        "stray-rejection-same-call.js",
        EvalMode::AsyncScript);
    check(
        !strayRejected.ok() &&
            strayRejected.reason == "promise-rejection" &&
            strayRejected.error.message == "same-call",
        "stray rejection is attributed to the call that created it");

    auto afterStray = context->evaluateAwaited(
        "await 0; 1",
        "after-stray-rejection.js",
        EvalMode::AsyncScript);
    check(
        afterStray.ok() && afterStray.value.has_value() &&
            number(*afterStray.value) == 1,
        "stray rejection does not contaminate the next awaited call");

    auto conversionWithStray = context->evaluateAwaited(
        "await 0; ({ get x() {"
        "  Promise.reject(new Error('conversion-stray'));"
        "  return new Date();"
        "} })",
        "conversion-with-stray-rejection.js",
        EvalMode::AsyncScript);
    check(
        !conversionWithStray.ok() &&
            conversionWithStray.reason == "value-conversion",
        "conversion failure remains the primary result over a stray rejection");

    auto afterConversionStray = context->evaluateAwaited(
        "await 0; 2",
        "after-conversion-stray.js",
        EvalMode::AsyncScript);
    check(
        afterConversionStray.ok() &&
            afterConversionStray.value.has_value() &&
            number(*afterConversionStray.value) == 2,
        "conversion-time stray rejection does not contaminate the next call");
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
        "});"
        "void 0;",
        "retain-global-setup.js");
    check(setup.ok(), "retainGlobal deadline setup succeeds");

    const auto started = std::chrono::steady_clock::now();
    bool threw = false;
    bool classified = false;
    try {
      (void)context->retainGlobal("slowGlobal");
    } catch (const QuickJSExecutionException& error) {
      threw = true;
      classified =
          error.reason() == "deadline" && error.code() == 1002;
    } catch (const std::exception&) {
      threw = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    check(threw, "retainGlobal interrupts guest getter");
    check(
        classified,
        "retainGlobal preserves deadline classification");
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
    bool classified = false;
    try {
      (void)context->retainEvaluation(
          "throw { toString() { for (;;) {} } }",
          "retain-error-stringify.js");
    } catch (const QuickJSExecutionException& error) {
      threw = true;
      classified =
          error.reason() == "deadline" && error.code() == 1002;
    } catch (const std::exception&) {
      threw = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    check(threw, "retainEvaluation reports thrown guest value");
    check(
        classified,
        "retainEvaluation preserves deadline classification");
    check(
        elapsed.count() < 200,
        "retainEvaluation exception stringification remains deadline-bounded");
  }

  {
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    QuickJSContext::AsyncHostCompletion completeLater;
    bool settledJobRan = false;

    context->registerAsyncHostFunction(
        "lateObjectForPendingJobs",
        [&completeLater](
            const std::vector<Value>&,
            QuickJSContext::AsyncHostCompletion complete) {
          completeLater = std::move(complete);
        });
    context->registerHostFunction(
        "disposeDuringResolution",
        [&context](const std::vector<Value>&) -> Value {
          context->dispose();
          return Value{true};
        });
    context->registerHostFunction(
        "markPendingJobRan",
        [&settledJobRan](const std::vector<Value>&) -> Value {
          settledJobRan = true;
          return Value{true};
        });

    auto setup = context->evaluate(
        "Object.defineProperty(Object.prototype, 'then', {"
        "  configurable: true,"
        "  get() {"
        "    delete Object.prototype.then;"
        "    disposeDuringResolution();"
        "    return undefined;"
        "  }"
        "});"
        "lateObjectForPendingJobs().then(() => markPendingJobRan());"
        "void 0;",
        "pending-jobs-dispose-setup.js");
    check(
        setup.ok() && static_cast<bool>(completeLater),
        "pending-jobs disposal regression setup succeeds");

    QuickJSContext::AsyncHostResult completion;
    Value::Object payload;
    payload["answer"] = Value{42};
    completion.value = Value{std::move(payload)};
    completeLater(std::move(completion));

    auto jobs = context->executePendingJobs();
    check(
        jobs.ok() && settledJobRan,
        "executePendingJobs stays pinned through completion and queued jobs");
    check(
        !context->isOpen(),
        "executePendingJobs releases deferred disposal only after the call");
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
        "outerObjectReentrant",
        [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion complete) {
          QuickJSContext::AsyncHostResult result;
          Value::Object value;
          value["answer"] = Value{42};
          result.value = Value{std::move(value)};
          complete(std::move(result));
        });
    context->registerAsyncHostFunction(
        "neverNestedReentrant",
        [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion) {});
    context->registerHostFunction(
        "nestedPendingClear",
        [&context](const std::vector<Value>&) -> Value {
          auto nested = context->evaluate(
              "await neverNestedReentrant();",
              "nested-pending-clear.js",
              EvalMode::AsyncScript);
          return Value{nested.reason};
        });

    auto reentrant = context->evaluateAwaited(
        "Object.defineProperty(Object.prototype, 'then', {"
        "  configurable: true,"
        "  get() {"
        "    delete Object.prototype.then;"
        "    globalThis.nestedClearReason = nestedPendingClear();"
        "    return undefined;"
        "  }"
        "});"
        "const value = await outerObjectReentrant();"
        "globalThis.reentrantOuterAnswer = value.answer;",
        "async-completion-reentrant-clear.js",
        EvalMode::AsyncScript);
    check(
        reentrant.ok(),
        "async completion survives reentrant pending-resolver clearing");
    auto reentrantState = context->evaluate(
        "({ answer: globalThis.reentrantOuterAnswer,"
        "   reason: globalThis.nestedClearReason })",
        "async-completion-reentrant-check.js");
    check(
        reentrantState.ok() && reentrantState.value.has_value() &&
            number(object(*reentrantState.value).at("answer")) == 42 &&
            std::get<std::string>(
                object(*reentrantState.value).at("reason").data) ==
                "pending-promise",
        "reentrant nested pending evaluation cannot invalidate outer resolver");
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
    QuickJSRuntime runtime;
    auto context = runtime.createContext();
    context->registerAsyncHostFunction(
        "throwingHost",
        [](const std::vector<Value>&, QuickJSContext::AsyncHostCompletion) {
          throw std::runtime_error("host callback threw");
        });

    auto awaited = context->evaluateAwaited(
        "try { await throwingHost(); } catch (error) { "
        "globalThis.thrownHostError = error.name + ':' + error.message; }",
        "async-host-throw.js",
        EvalMode::AsyncScript);
    check(
        awaited.ok(),
        "thrown async host exception becomes a catchable Promise rejection");

    auto error =
        context->evaluate("globalThis.thrownHostError", "host-throw-error.js");
    check(
        error.ok() && error.value.has_value() &&
            std::get<std::string>(error.value->data) ==
                "Error:host callback threw",
        "thrown async host exception preserves its message");
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
    options.maxStackBytes = 32 * 1024 * 1024;
    QuickJSRuntime runtime(options);
    check(
        runtime.maxStackBytes() < 32 * 1024 * 1024,
        "QuickJS stack limit is clamped below the native thread stack");
    runtime.setMaxStackBytes(32 * 1024 * 1024);
    check(
        runtime.maxStackBytes() < 32 * 1024 * 1024,
        "later QuickJS stack-limit updates remain native-stack bounded");
  }

#if defined(__linux__)
  {
    pthread_attr_t attributes;
    SmallStackResult smallStack;
    pthread_t thread{};
    const bool attributesReady = pthread_attr_init(&attributes) == 0;
    const bool stackConfigured =
        attributesReady &&
        pthread_attr_setstacksize(&attributes, 512 * 1024) == 0;
    const bool threadStarted =
        stackConfigured &&
        pthread_create(
            &thread,
            &attributes,
            &runQuickJSOnSmallNativeStack,
            &smallStack) == 0;
    if (attributesReady) {
      pthread_attr_destroy(&attributes);
    }
    if (threadStarted) {
      pthread_join(thread, nullptr);
    }
    check(
        threadStarted && smallStack.runtimeCreated,
        "QuickJS runtime starts on a 512 KiB native thread stack");
    check(
        threadStarted && smallStack.stackWasClamped,
        "QuickJS logical stack stays below a 512 KiB native stack");
    check(
        threadStarted && smallStack.recursionWasCaught,
        "deep guest recursion is caught before native stack exhaustion");
  }
#endif

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

    auto asyncModule = context->evaluateAwaited(
        "import { answer } from 'math';"
        "await Promise.resolve();"
        "globalThis.asyncModuleAnswer = answer + 1;",
        "async-entry.mjs",
        EvalMode::AsyncModule);
    check(asyncModule.ok(), "async module with top-level await settles");
    auto asyncValue =
        context->evaluate("asyncModuleAnswer", "async-module-check.js");
    check(
        asyncValue.ok() && asyncValue.value.has_value() &&
            number(*asyncValue.value) == 43,
        "async module side effect visible after top-level await");

    runtime.removeModule("math");
    auto cached = context->evaluate(
        "import { answer } from 'math'; globalThis.cachedModuleAnswer = answer;",
        "cached-entry.mjs",
        EvalMode::Module);
    check(
        cached.ok(),
        "already-loaded module remains cached after source removal");
    auto freshContext = runtime.createContext();
    auto removedForFreshContext = freshContext->evaluate(
        "import { answer } from 'math'; globalThis.noLongerAvailable = answer;",
        "fresh-context-after-remove.mjs",
        EvalMode::Module);
    check(
        !removedForFreshContext.ok() &&
            removedForFreshContext.reason == "module-denied",
        "removed module is denied for future resolution in a fresh context");

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

    auto conversionFailure = context->evaluate(
        "try {"
        "  console.log({ toString() { throw new Error('console conversion'); } });"
        "  'not-caught';"
        "} catch (error) {"
        "  error.message;"
        "}",
        "console-conversion-error.js");
    check(
        conversionFailure.ok() && conversionFailure.value.has_value() &&
            std::get<std::string>(conversionFailure.value->data) ==
                "console conversion",
        "console.log propagates guest string-conversion failures");
  }

  {
    RuntimeOptions options;
    options.maxOutputBytes = 8;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();
    auto logged = context->evaluate(
        "console.log('abcdefghijklmnopqrstuvwxyz'); 'ok';",
        "console-output-bound.js");
    check(logged.ok(), "bounded console script succeeds");
    check(
        context->getOutput().size() <= options.maxOutputBytes,
        "console conversion never retains more than the configured output bytes");
    check(
        context->outputWasTruncated(),
        "oversized console conversion reports truncated output");
    (void)context->takeOutput();
    check(
        !context->outputWasTruncated(),
        "draining all output resets truncation state");
    auto shortLogged = context->evaluate(
        "console.log('ok'); 'ok';",
        "console-output-after-drain.js");
    check(
        shortLogged.ok() && !context->outputWasTruncated(),
        "short output after a full drain is not marked truncated");
  }

  {
    RuntimeOptions options;
    options.maxOutputBytes = 2;
    options.maxOutputLines = 10;
    QuickJSRuntime runtime(options);
    auto context = runtime.createContext();
    auto logged = context->evaluate(
        "console.log(); console.log(); console.log(); console.log(); 'ok';",
        "console-separator-bound.js");
    check(logged.ok(), "empty console lines respect output accounting");
    check(
        context->getOutput().size() <= options.maxOutputBytes,
        "serialized console separators count against the output byte limit");
    check(
        context->outputWasTruncated(),
        "console separator overflow reports truncated output");
    const auto taken = context->takeOutput(1);
    check(
        taken.empty() && context->getOutput().size() <= options.maxOutputBytes,
        "taking output preserves serialized byte accounting");
    check(
        context->outputWasTruncated(),
        "partial output drain preserves truncation state");
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
