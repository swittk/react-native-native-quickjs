# react-native-native-quickjs

A small JSI interface to an isolated native QuickJS VM for React Native 0.73.6
and newer.

This package vendors and directly compiles the official **QuickJS 2026-06-04**
C sources. It has no third-party QuickJS wrapper dependency and does not link a
separately installed QuickJS library. The upstream `quickjs-libc` source is
retained in the vendor snapshot but is never compiled or linked, so the guest
VM has no ambient filesystem, OS, process, or network API.

Hermes remains the React Native host runtime in a normal Hermes application.
QuickJS is a separate guest VM reached through JSI; installing this package does
not replace Hermes or change React Native's JavaScript engine.

## Installation

```sh
pnpm add react-native-native-quickjs
```

Run `pod install` for iOS and rebuild the native application. Expo Go cannot
load custom native code, so Expo projects need a development/native build. The
iOS deployment floor is 13.4.

## Recommended: worker runtime

For user-authored or potentially long-running code, use
`createQuickJSWorker()`. The QuickJS runtime and context are created and owned
by a dedicated native worker thread, so an infinite loop does not block Hermes.

```ts
import {createQuickJSWorker} from 'react-native-native-quickjs';

const worker = createQuickJSWorker({
  // No execution deadline by default. Add a positive executionLimitMs only
  // when the embedder explicitly wants automatic timeout interruption.
  memoryLimitBytes: 16 * 1024 * 1024,
  maxStackBytes: 1024 * 1024,
});

await worker.registerAsyncHostFunction('takePhoto', async () => {
  return await openNativePhotoPicker();
});

const result = await worker.evaluateAsync(`
  const photo = await takePhoto();
  photo;
`);

worker.dispose();
```

An async host callback may return either a normal value or a Hermes Promise.
QuickJS always receives a real QuickJS Promise, so guest code can use ordinary
JavaScript:

```js
const photo = await takePhoto();
const [a, b] = await Promise.all([readA(), readB()]);
```

The worker automatically pumps QuickJS pending jobs. Guest programs never call
`executePendingJobs()`.

### Hard cancellation

`worker.cancel()` is thread-safe. It uses QuickJS's native
`JS_SetInterruptHandler` and wakes a VM that is either executing JavaScript or
idle while awaiting a host Promise.

```ts
const running = worker.evaluateAsync('for (;;) {}', {mode: 'script'});

setTimeout(() => worker.cancel(), 10);

const result = await running;
// result.reason === 'cancelled'
```

The VM stays on its owner thread; cancellation only flips/wakes native atomic
state. QuickJS contexts are never migrated between threads.

### Retained async callbacks

Retained functions remain compiled inside QuickJS and can themselves await host
Promises:

```ts
const handle = await worker.retainAsync(
  'async value => await hostTransform(value)'
);

const result = await worker.callAsync(handle, [42]);
worker.release(handle);
```

This is the intended primitive for persistent handlers, frame callbacks, and
other hot persistent callbacks.

## Synchronous embedding API

`createQuickJSRuntime()` remains available for low-level embedding, debugging,
and short synchronous work.

```ts
import {createQuickJSRuntime} from 'react-native-native-quickjs';

const runtime = createQuickJSRuntime({
  executionLimitMs: 250,
  memoryLimitBytes: 16 * 1024 * 1024,
});
const context = runtime.createContext();

context.registerHostFunction('hostAdd', (a, b) => Number(a) + Number(b));

const result = context.evaluate(`
  ({answer: hostAdd(20, 22), values: [1, true, null]});
`);

context.dispose();
runtime.dispose();
```

The synchronous context exposes `executePendingJobs()` intentionally because it
is a low-level embedding API. Prefer the worker runtime for untrusted or
long-running user-authored scripts.

## Values and limits

Values crossing the bridge are limited to `undefined`, `null`, booleans,
numbers, strings, arrays, and plain objects. Conversion has depth and node
bounds. Raw `JSValue`, `JSContext`, `JSRuntime`, and other QuickJS pointers
are never exposed to React Native JavaScript.

Runtime options include:

- optional execution deadline per active JavaScript turn (`0`/omitted means
  unlimited, which is the intentional default);
- memory limit;
- maximum stack size (treated as an upper bound and clamped below the actual native thread stack with host-frame headroom);
- bounded console output.

Automatic execution deadlines are **opt-in**. The library intentionally does
not impose a default time limit: callers may run a guest loop indefinitely and
control its lifetime explicitly with `worker.cancel()` or runtime cancellation.
Set a positive `executionLimitMs` only when the embedder wants an automatic
per-turn timeout.

Per-result memory snapshots are also opt-in. `JS_ComputeMemoryUsage` walks the
QuickJS heap, so `collectResultMemoryStats` defaults to `false` to keep retained
callback and async-resume paths fast. `runtime.memory` and
`await worker.memoryAsync()` remain available for explicit on-demand diagnostics.

Time spent waiting for an external host Promise does **not** consume a configured
JavaScript execution deadline. When the Promise settles, the resumed QuickJS
turn receives a fresh CPU budget.

## Modules

Modules are supplied explicitly from memory:

```ts
worker.addModule('app:math', 'export const answer = 42;');

await worker.evaluateAsync(
  `import {answer} from 'app:math'; globalThis.answer = answer;`,
  {filename: 'main.mjs', mode: 'module'},
);
```

An import that was not registered is denied. There is no fallback to disk or
the network. `removeModule()` and `clearModules()` remove sources from future
resolution; modules already loaded by an existing QuickJS context remain in
that context's normal module cache.

## Architecture support

The package keeps the RN 0.73 legacy bridge installation path and uses an
explicit synchronous `installBindings()` TurboModule path for New Architecture
instead of version-sensitive JSI installer protocols. Both install the same JSI
factories:

- `SKRNNativeQuickJSCreateRuntime`
- `SKRNNativeQuickJSCreateWorker`

Both use the same vendored QuickJS core.

React Native 0.73.6 support covers its legacy architecture and its normal New
Architecture bridge path. RN 0.73's separately gated experimental
`enableBridgelessArchitecture` mode does not expose a public JavaScript runtime
holder and is not part of the 0.73 compatibility matrix.

The example remains on React Native 0.73.6 with Hermes and demonstrates both the
synchronous embedding surface and the worker/await/hard-cancel surface.

## Development

```sh
pnpm test:native
pnpm typescript
```

The native test compiles `quickjs.c`, `dtoa.c`, `libregexp.c`,
`libunicode.c`, and `cutils.c` directly with upstream definitions
(`_GNU_SOURCE`, `CONFIG_VERSION="2026-06-04"`, and `-fwrapv`).

The native suite covers async host Promise settlement, `Promise.all`, host
rejection, retained async callbacks, cancellation while awaiting a host
operation, cancellation cleanup, deadlines, memory limits, modules, and
runtime/context lifecycle.

## License

MIT. QuickJS is MIT licensed; its upstream license is retained in
`vendor/QUICKJS_LICENSE` and `vendor/quickjs/LICENSE`.
