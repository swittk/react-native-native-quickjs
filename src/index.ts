import {Platform} from 'react-native';

import NativeQuickJS from './NativeQuickJS';

const LINKING_ERROR =
  `The package 'react-native-native-quickjs' does not appear to be linked.\n\n` +
  Platform.select({ios: "Run 'pod install' and rebuild the app.\n", default: ''}) +
  'A custom native build is required; Expo Go cannot load this package.';

export type QuickJSValue =
  | undefined
  | null
  | boolean
  | number
  | string
  | QuickJSValue[]
  | {[key: string]: QuickJSValue};

export type QuickJSEvalMode =
  | 'script'
  | 'module'
  | 'async-script'
  | 'async-module';

export type QuickJSExecutionReason =
  | 'ok'
  | 'runtime'
  | 'syntax'
  | 'deadline'
  | 'cancelled'
  | 'memory-limit'
  | 'destroyed'
  | 'invalid-handle'
  | 'job-limit'
  | 'promise-rejection'
  | 'pending-promise'
  | 'module-denied'
  | 'value-conversion';

export interface QuickJSRuntimeOptions {
  /**
   * Optional per-turn guest execution deadline in milliseconds.
   *
   * Defaults to 0 (unlimited). Set a positive value to opt into automatic
   * deadline interruption. `worker.cancel()` / runtime cancellation remains
   * available even when no automatic deadline is configured.
   */
  executionLimitMs?: number;
  memoryLimitBytes?: number;
  maxStackBytes?: number;
  maxOutputBytes?: number;
  maxOutputLines?: number;
  /**
   * Include a full QuickJS heap snapshot in every execution result.
   *
   * Defaults to false because collecting it walks the QuickJS heap. Use
   * runtime.memory for on-demand diagnostics without taxing hot call paths.
   */
  collectResultMemoryStats?: boolean;
}

export interface QuickJSEvaluateOptions {
  filename?: string;
  mode?: QuickJSEvalMode;
}

export interface QuickJSRetainOptions {
  /** Treat the first argument as a global function name instead of source. */
  global?: boolean;
  filename?: string;
}

export interface QuickJSErrorInfo {
  name: string;
  message: string;
  stack: string;
}

export interface QuickJSMemoryStats {
  mallocBytes: number;
  memoryUsedBytes: number;
  mallocCount: number;
  objectCount: number;
  atomCount: number;
}

export interface QuickJSExecutionResult<T extends QuickJSValue = QuickJSValue> {
  reason: QuickJSExecutionReason;
  code: number;
  value: T | undefined;
  error: QuickJSErrorInfo;
  durationMs: number;
  memory: QuickJSMemoryStats;
  outputTruncated: boolean;
}

export type QuickJSHostFunction = (
  ...args: QuickJSValue[]
) => QuickJSValue;

export type QuickJSAsyncHostFunction = (
  ...args: QuickJSValue[]
) => QuickJSValue | Promise<QuickJSValue>;

export interface QuickJSWorkerResult<T extends QuickJSValue = QuickJSValue>
  extends QuickJSExecutionResult<T> {
  output: string;
}

interface NativeQuickJSContext {
  readonly valid: boolean;
  readonly outputCount: number;
  readonly outputTruncated: boolean;
  evaluate(
    source: string,
    options?: QuickJSEvaluateOptions
  ): QuickJSExecutionResult;
  retain(sourceOrGlobal: string, options?: QuickJSRetainOptions): number;
  call(handle: number, args?: QuickJSValue[]): QuickJSExecutionResult;
  release(handle: number): void;
  executePendingJobs(maxJobs?: number): QuickJSExecutionResult;
  getOutput(count?: number): string;
  takeOutput(count?: number): string;
  registerHostFunction(name: string, callback: QuickJSHostFunction): void;
  dispose(): void;
}

interface NativeQuickJSWorkerTaskResult extends QuickJSWorkerResult {
  taskId: number;
}

interface NativeQuickJSWorker {
  readonly valid: boolean;
  readonly executing: boolean;
  startEvaluate(source: string, options?: QuickJSEvaluateOptions): number;
  startRetain(sourceOrGlobal: string, options?: QuickJSRetainOptions): number;
  startCall(handle: number, args?: QuickJSValue[]): number;
  startMemory(): number;
  takeTaskResult(taskId: number): QuickJSWorkerResult | null;
  takeNextTaskResult(): NativeQuickJSWorkerTaskResult | null;
  release(handle: number): void;
  registerAsyncHostFunction(
    name: string,
    callback: QuickJSAsyncHostFunction
  ): number;
  addModule(name: string, source: string): void;
  removeModule(name: string): void;
  clearModules(): void;
  cancel(): void;
  dispose(): void;
}

interface NativeQuickJSRuntime {
  readonly valid: boolean;
  readonly executionLimitMs: number;
  readonly memoryLimitBytes: number;
  readonly maxStackBytes: number;
  readonly memory: QuickJSMemoryStats;
  createContext(): NativeQuickJSContext;
  addModule(name: string, source: string): void;
  removeModule(name: string): void;
  clearModules(): void;
  cancel(): void;
  resetCancellation(): void;
  setExecutionLimitMs(ms: number): void;
  setMemoryLimitBytes(bytes: number): void;
  setMaxStackBytes(bytes: number): void;
  dispose(): void;
}

type QuickJSFactory = (
  options?: QuickJSRuntimeOptions
) => NativeQuickJSRuntime;

type QuickJSWorkerFactory = (
  options?: QuickJSRuntimeOptions
) => NativeQuickJSWorker;

declare global {
  // Installed by the legacy bridge adapter or New Architecture bindings hook.
  // eslint-disable-next-line no-var
  var SKRNNativeQuickJSCreateRuntime: QuickJSFactory | undefined;
  // Installed beside the synchronous embedding factory.
  // eslint-disable-next-line no-var
  var SKRNNativeQuickJSCreateWorker: QuickJSWorkerFactory | undefined;
}

export interface QuickJSContext {
  readonly valid: boolean;
  readonly outputCount: number;
  readonly outputTruncated: boolean;
  evaluate<T extends QuickJSValue = QuickJSValue>(
    source: string,
    options?: QuickJSEvaluateOptions
  ): QuickJSExecutionResult<T>;
  evalModule<T extends QuickJSValue = QuickJSValue>(
    source: string,
    options?: Omit<QuickJSEvaluateOptions, 'mode'>
  ): QuickJSExecutionResult<T>;
  evalAsyncScript<T extends QuickJSValue = QuickJSValue>(
    source: string,
    options?: Omit<QuickJSEvaluateOptions, 'mode'>
  ): QuickJSExecutionResult<T>;
  retain(sourceOrGlobal: string, options?: QuickJSRetainOptions): number;
  call<T extends QuickJSValue = QuickJSValue>(
    handle: number,
    args?: QuickJSValue[]
  ): QuickJSExecutionResult<T>;
  release(handle: number): void;
  executePendingJobs(maxJobs?: number): QuickJSExecutionResult;
  getOutput(count?: number): string;
  takeOutput(count?: number): string;
  registerHostFunction(name: string, callback: QuickJSHostFunction): void;
  dispose(): void;
}

export interface QuickJSWorker {
  readonly valid: boolean;
  readonly executing: boolean;

  /**
   * Executes entirely on the native QuickJS worker. By default this uses
   * async-script mode, so top-level await works naturally.
   */
  evaluateAsync<T extends QuickJSValue = QuickJSValue>(
    source: string,
    options?: QuickJSEvaluateOptions
  ): Promise<QuickJSWorkerResult<T>>;

  retainAsync(
    sourceOrGlobal: string,
    options?: QuickJSRetainOptions
  ): Promise<number>;

  callAsync<T extends QuickJSValue = QuickJSValue>(
    handle: number,
    args?: QuickJSValue[]
  ): Promise<QuickJSWorkerResult<T>>;

  /** Collect a full QuickJS heap snapshot on the native worker thread. */
  memoryAsync(): Promise<QuickJSMemoryStats>;

  release(handle: number): void;

  /**
   * The callback may return a value or a Hermes Promise. Guest QuickJS receives
   * a real QuickJS Promise either way, so await/Promise.all work normally.
   */
  registerAsyncHostFunction(
    name: string,
    callback: QuickJSAsyncHostFunction
  ): Promise<void>;

  addModule(name: string, source: string): void;
  removeModule(name: string): void;
  clearModules(): void;

  /** Thread-safe hard interrupt for active JavaScript or an idle host await. */
  cancel(): void;
  dispose(): void;
}

export interface QuickJSRuntime {
  readonly valid: boolean;
  readonly executionLimitMs: number;
  readonly memoryLimitBytes: number;
  readonly maxStackBytes: number;
  readonly memory: QuickJSMemoryStats;
  createContext(): QuickJSContext;
  addModule(name: string, source: string): void;
  removeModule(name: string): void;
  clearModules(): void;
  cancel(): void;
  resetCancellation(): void;
  setExecutionLimitMs(ms: number): void;
  setMemoryLimitBytes(bytes: number): void;
  setMaxStackBytes(bytes: number): void;
  dispose(): void;
}

function wrapContext(native: NativeQuickJSContext): QuickJSContext {
  // ContextHostObject materializes methods through its JSI property trap.
  // Cache those stable method functions once so hot calls do not allocate a
  // fresh host-function wrapper on every property access.
  let evaluateNative: NativeQuickJSContext['evaluate'] | undefined;
  let retainNative: NativeQuickJSContext['retain'] | undefined;
  let callNative: NativeQuickJSContext['call'] | undefined;
  let releaseNative: NativeQuickJSContext['release'] | undefined;
  let executePendingJobsNative:
    | NativeQuickJSContext['executePendingJobs']
    | undefined;
  let getOutputNative: NativeQuickJSContext['getOutput'] | undefined;
  let takeOutputNative: NativeQuickJSContext['takeOutput'] | undefined;
  let registerHostFunctionNative:
    | NativeQuickJSContext['registerHostFunction']
    | undefined;
  let disposeNative: NativeQuickJSContext['dispose'] | undefined;

  return {
    get valid() {
      return native.valid;
    },
    get outputCount() {
      return native.outputCount;
    },
    get outputTruncated() {
      return native.outputTruncated;
    },
    evaluate<T extends QuickJSValue = QuickJSValue>(
      source: string,
      options?: QuickJSEvaluateOptions
    ): QuickJSExecutionResult<T> {
      return (evaluateNative ??= native.evaluate)(source, options) as QuickJSExecutionResult<T>;
    },
    evalModule<T extends QuickJSValue = QuickJSValue>(
      source: string,
      options: Omit<QuickJSEvaluateOptions, 'mode'> = {}
    ): QuickJSExecutionResult<T> {
      return (evaluateNative ??= native.evaluate)(source, {
        ...options,
        mode: 'module',
      }) as QuickJSExecutionResult<T>;
    },
    evalAsyncScript<T extends QuickJSValue = QuickJSValue>(
      source: string,
      options: Omit<QuickJSEvaluateOptions, 'mode'> = {}
    ): QuickJSExecutionResult<T> {
      return (evaluateNative ??= native.evaluate)(source, {
        ...options,
        mode: 'async-script',
      }) as QuickJSExecutionResult<T>;
    },
    retain: (sourceOrGlobal, options) =>
      (retainNative ??= native.retain)(sourceOrGlobal, options),
    call<T extends QuickJSValue = QuickJSValue>(
      handle: number,
      args?: QuickJSValue[]
    ): QuickJSExecutionResult<T> {
      return (callNative ??= native.call)(handle, args) as QuickJSExecutionResult<T>;
    },
    release: handle => (releaseNative ??= native.release)(handle),
    executePendingJobs: maxJobs =>
      (executePendingJobsNative ??= native.executePendingJobs)(maxJobs),
    getOutput: count => (getOutputNative ??= native.getOutput)(count),
    takeOutput: count => (takeOutputNative ??= native.takeOutput)(count),
    registerHostFunction: (name, callback) =>
      (registerHostFunctionNative ??= native.registerHostFunction)(name, callback),
    dispose: () => (disposeNative ??= native.dispose)(),
  };
}

const WORKER_POLL_INTERVAL_MS = 8;

interface PendingWorkerTask {
  resolve(result: QuickJSWorkerResult): void;
  reject(error: unknown): void;
}

function createWorkerTaskAwaiter(worker: NativeQuickJSWorker) {
  // WorkerHostObject creates method functions from its JSI property trap.
  // Retain this hot polling method once instead of allocating a fresh host
  // function on every poll.
  const takeNextTaskResult = worker.takeNextTaskResult;
  const pending = new Map<number, PendingWorkerTask>();
  let pollTimer: ReturnType<typeof setTimeout> | undefined;

  const rejectAll = (error: unknown): void => {
    if (pollTimer !== undefined) {
      clearTimeout(pollTimer);
      pollTimer = undefined;
    }
    for (const waiter of pending.values()) {
      waiter.reject(error);
    }
    pending.clear();
  };

  const poll = (): void => {
    pollTimer = undefined;
    try {
      while (true) {
        const result = takeNextTaskResult();
        if (!result) {
          break;
        }
        const waiter = pending.get(result.taskId);
        if (waiter) {
          pending.delete(result.taskId);
          waiter.resolve(result);
        }
      }

      if (pending.size === 0) {
        return;
      }
      if (!worker.valid) {
        rejectAll(
          new Error('QuickJS worker was disposed before task completion.')
        );
        return;
      }
      pollTimer = setTimeout(poll, WORKER_POLL_INTERVAL_MS);
    } catch (error) {
      rejectAll(error);
    }
  };

  const schedulePoll = (): void => {
    if (pollTimer === undefined) {
      pollTimer = setTimeout(poll, 0);
    }
  };

  return {
    wait<T extends QuickJSValue = QuickJSValue>(
      taskId: number
    ): Promise<QuickJSWorkerResult<T>> {
      return new Promise((resolve, reject) => {
        if (pending.has(taskId)) {
          reject(new Error('QuickJS worker task is already being awaited.'));
          return;
        }
        pending.set(taskId, {
          resolve: result => resolve(result as QuickJSWorkerResult<T>),
          reject,
        });
        schedulePoll();
      });
    },
    dispose(): void {
      rejectAll(
        new Error('QuickJS worker was disposed before task completion.')
      );
    },
  };
}

function throwWorkerResult(result: QuickJSWorkerResult): never {
  const message =
    result.error.message ||
    ('QuickJS worker task failed with reason ' + result.reason + '.');
  const error = new Error(message);
  error.name = result.error.name || 'QuickJSError';
  if (result.error.stack) {
    error.stack = result.error.stack;
  }
  throw error;
}

function ensureJSIBindings(): void {
  if (!NativeQuickJS) {
    throw new Error(LINKING_ERROR);
  }
  if (
    typeof globalThis.SKRNNativeQuickJSCreateRuntime === 'function' &&
    typeof globalThis.SKRNNativeQuickJSCreateWorker === 'function'
  ) {
    return;
  }

  // Install synchronously from the React Native JS thread. This explicit
  // TurboModule method is used across architectures so the package does not
  // depend on version-specific JSI installer protocols.
  if (!NativeQuickJS.installBindings()) {
    throw new Error(
      "QuickJS JSI bindings could not access the React Native runtime."
    );
  }
}

/**
 * Creates a QuickJS runtime/context owned by a dedicated native worker thread.
 * This is the recommended execution surface for untrusted/user-authored apps.
 */
export function createQuickJSWorker(
  options?: QuickJSRuntimeOptions
): QuickJSWorker {
  ensureJSIBindings();
  if (typeof globalThis.SKRNNativeQuickJSCreateWorker !== 'function') {
    throw new Error(LINKING_ERROR);
  }

  const native = globalThis.SKRNNativeQuickJSCreateWorker(options);
  // JSI HostObject methods are materialized by property access. Cache worker
  // methods once so each task does not allocate a fresh host function wrapper.
  let startEvaluate: NativeQuickJSWorker['startEvaluate'] | undefined;
  let startRetain: NativeQuickJSWorker['startRetain'] | undefined;
  let startCall: NativeQuickJSWorker['startCall'] | undefined;
  let startMemory: NativeQuickJSWorker['startMemory'] | undefined;
  let release: NativeQuickJSWorker['release'] | undefined;
  let registerNativeAsyncHostFunction:
    | NativeQuickJSWorker['registerAsyncHostFunction']
    | undefined;
  let addModule: NativeQuickJSWorker['addModule'] | undefined;
  let removeModule: NativeQuickJSWorker['removeModule'] | undefined;
  let clearModules: NativeQuickJSWorker['clearModules'] | undefined;
  let cancel: NativeQuickJSWorker['cancel'] | undefined;
  let disposeNative: NativeQuickJSWorker['dispose'] | undefined;
  const taskAwaiter = createWorkerTaskAwaiter(native);
  return {
    get valid() {
      return native.valid;
    },
    get executing() {
      return native.executing;
    },
    async evaluateAsync<T extends QuickJSValue = QuickJSValue>(
      source: string,
      options?: QuickJSEvaluateOptions
    ): Promise<QuickJSWorkerResult<T>> {
      return taskAwaiter.wait<T>(
        (startEvaluate ??= native.startEvaluate)(source, options)
      );
    },
    async retainAsync(
      sourceOrGlobal: string,
      retainOptions?: QuickJSRetainOptions
    ): Promise<number> {
      const result = await taskAwaiter.wait(
        (startRetain ??= native.startRetain)(sourceOrGlobal, retainOptions)
      );
      if (result.reason !== 'ok') {
        throwWorkerResult(result);
      }
      if (typeof result.value !== 'number') {
        throw new Error('QuickJS worker did not return a retained handle.');
      }
      return result.value;
    },
    async callAsync<T extends QuickJSValue = QuickJSValue>(
      handle: number,
      args?: QuickJSValue[]
    ): Promise<QuickJSWorkerResult<T>> {
      return taskAwaiter.wait<T>((startCall ??= native.startCall)(handle, args));
    },
    async memoryAsync(): Promise<QuickJSMemoryStats> {
      const result = await taskAwaiter.wait((startMemory ??= native.startMemory)());
      if (result.reason !== 'ok') {
        throwWorkerResult(result);
      }
      return result.memory;
    },
    release: handle => (release ??= native.release)(handle),
    async registerAsyncHostFunction(name, callback): Promise<void> {
      const result = await taskAwaiter.wait(
        (registerNativeAsyncHostFunction ??= native.registerAsyncHostFunction)(name, callback)
      );
      if (result.reason !== 'ok') {
        throwWorkerResult(result);
      }
    },
    addModule: (name, source) => (addModule ??= native.addModule)(name, source),
    removeModule: name => (removeModule ??= native.removeModule)(name),
    clearModules: () => (clearModules ??= native.clearModules)(),
    cancel: () => (cancel ??= native.cancel)(),
    dispose: () => {
      try {
        (disposeNative ??= native.dispose)();
      } finally {
        taskAwaiter.dispose();
      }
    },
  };
}

/** Creates one native QuickJS VM. Contexts share its modules and limits. */
export function createQuickJSRuntime(
  options?: QuickJSRuntimeOptions
): QuickJSRuntime {
  ensureJSIBindings();
  if (typeof globalThis.SKRNNativeQuickJSCreateRuntime !== 'function') {
    throw new Error(LINKING_ERROR);
  }

  const native = globalThis.SKRNNativeQuickJSCreateRuntime(options);
  return {
    get valid() {
      return native.valid;
    },
    get executionLimitMs() {
      return native.executionLimitMs;
    },
    get memoryLimitBytes() {
      return native.memoryLimitBytes;
    },
    get maxStackBytes() {
      return native.maxStackBytes;
    },
    get memory() {
      return native.memory;
    },
    createContext: () => wrapContext(native.createContext()),
    addModule: (name, source) => native.addModule(name, source),
    removeModule: name => native.removeModule(name),
    clearModules: () => native.clearModules(),
    cancel: () => native.cancel(),
    resetCancellation: () => native.resetCancellation(),
    setExecutionLimitMs: ms => native.setExecutionLimitMs(ms),
    setMemoryLimitBytes: bytes => native.setMemoryLimitBytes(bytes),
    setMaxStackBytes: bytes => native.setMaxStackBytes(bytes),
    dispose: () => native.dispose(),
  };
}
