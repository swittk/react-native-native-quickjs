import * as React from 'react';
import {Button, ScrollView, StyleSheet, Text, View} from 'react-native';

import {
  createQuickJSRuntime,
  createQuickJSWorker,
  type QuickJSExecutionResult,
  type QuickJSRuntime,
  type QuickJSWorker,
} from 'react-native-native-quickjs';

function describe(name: string, result: QuickJSExecutionResult): string {
  const detail = result.reason === 'ok'
    ? JSON.stringify(result.value)
    : `${result.error.name || 'Error'}: ${result.error.message}`;
  return `${name}: ${result.reason} (${detail ?? 'undefined'})`;
}

export default function App() {
  const runtimeRef = React.useRef<QuickJSRuntime>();
  const workerRef = React.useRef<QuickJSWorker>();
  const workerRunRef = React.useRef(0);
  const [report, setReport] = React.useState('Press a demo button.');

  const runDemo = React.useCallback(() => {
    runtimeRef.current?.dispose();
    const runtime = createQuickJSRuntime({
      executionLimitMs: 100,
      memoryLimitBytes: 16 * 1024 * 1024,
      maxStackBytes: 1024 * 1024,
      maxOutputBytes: 16 * 1024,
      maxOutputLines: 128,
    });
    runtimeRef.current = runtime;
    const context = runtime.createContext();
    const lines: string[] = [];

    try {
      lines.push(describe('eval', context.evaluate('6 * 7')));

      context.registerHostFunction('hostAdd', (a, b) =>
        Number(a) + Number(b)
      );
      lines.push(describe('host function', context.evaluate('hostAdd(20, 22)')));

      const callback = context.retain(
        '(name, values) => `hello ${name}: ${values.join(",")}`'
      );
      lines.push(describe(
        'retained callback',
        context.call(callback, ['RN', [1, 2, 3]])
      ));
      context.release(callback);

      context.evaluate(`
        globalThis.promiseAnswer = 0;
        Promise.resolve(21).then(value => {
          globalThis.promiseAnswer = value * 2;
          console.log('promise', globalThis.promiseAnswer);
        });
      `);
      lines.push(describe('Promise jobs', context.executePendingJobs()));
      lines.push(describe(
        'Promise value',
        context.evaluate('globalThis.promiseAnswer')
      ));

      lines.push(describe(
        'async script compile',
        context.evalAsyncScript(
          'globalThis.asyncAnswer = await Promise.resolve(7 * 6);'
        )
      ));
      lines.push(describe('async script jobs', context.executePendingJobs()));

      runtime.cancel();
      lines.push(describe(
        'cancellation',
        context.evaluate('for (;;) {}', {filename: 'cancelled.js'})
      ));
      runtime.resetCancellation();

      runtime.setExecutionLimitMs(20);
      lines.push(describe(
        'deadline',
        context.evaluate('for (;;) {}', {filename: 'deadline.js'})
      ));
      runtime.setExecutionLimitMs(100);

      runtime.addModule(
        'demo:math',
        'export const answer = 42; export const twice = value => value * 2;'
      );
      lines.push(describe(
        'module import',
        context.evalModule(`
          import {answer, twice} from 'demo:math';
          globalThis.moduleAnswer = twice(answer / 2);
          console.log('module', globalThis.moduleAnswer);
        `, {filename: 'demo.mjs'})
      ));
      lines.push(describe(
        'module value',
        context.evaluate('globalThis.moduleAnswer')
      ));

      const captured = context.takeOutput();
      lines.push(`captured output:\n${captured || '(none)'}`);
      lines.push(`memory used: ${runtime.memory.memoryUsedBytes} bytes`);
    } catch (error) {
      lines.push(`bridge error: ${String(error)}`);
    } finally {
      context.dispose();
    }

    setReport(lines.join('\n'));
  }, []);

  const runWorkerDemo = React.useCallback(async () => {
    const runId = ++workerRunRef.current;
    workerRef.current?.dispose();
    const lines: string[] = [];
    let worker: QuickJSWorker;
    try {
      worker = createQuickJSWorker({
        executionLimitMs: 250,
        memoryLimitBytes: 16 * 1024 * 1024,
        maxStackBytes: 1024 * 1024,
        maxOutputBytes: 16 * 1024,
        maxOutputLines: 128,
      });
    } catch (error) {
      if (workerRunRef.current === runId) {
        setReport('worker bridge error: ' + String(error));
      }
      return;
    }
    workerRef.current = worker;

    // Hermes may return an ordinary value or Promise here. QuickJS always sees
    // a real Promise, so guest code can use normal await/Promise.all.
    worker.registerAsyncHostFunction('hostDelay', async (value, delayMs) => {
      await new Promise<void>(resolve => setTimeout(() => resolve(), Number(delayMs)));
      return Number(value) * 2;
    });

    try {
      const awaited = await worker.evaluateAsync(
        "const [a, b] = await Promise.all([" +
          "hostDelay(20, 20), hostDelay(1, 5)]);" +
          "console.log('awaited', a + b);" +
          "a + b;",
        {filename: 'worker-await.js'}
      );
      lines.push(describe('worker await', awaited));

      const callback = await worker.retainAsync(
        'async value => await hostDelay(value, 5)'
      );
      const called = await worker.callAsync(callback, [21]);
      lines.push(describe('retained async callback', called));
      worker.release(callback);

      const cancelledPromise = worker.evaluateAsync(
        'for (;;) {}',
        {filename: 'worker-cancel.js', mode: 'script'}
      );
      setTimeout(() => worker.cancel(), 10);
      const cancelled = await cancelledPromise;
      lines.push(describe('hard cancel', cancelled));

      lines.push('captured output: ' + (awaited.output || '(none)'));
    } catch (error) {
      lines.push('worker bridge error: ' + String(error));
    } finally {
      worker.dispose();
      if (workerRef.current === worker) workerRef.current = undefined;
    }

    if (workerRunRef.current === runId) {
      setReport(lines.join('\n'));
    }
  }, []);

  React.useEffect(() => () => {
    workerRunRef.current += 1;
    runtimeRef.current?.dispose();
    runtimeRef.current = undefined;
    workerRef.current?.dispose();
    workerRef.current = undefined;
  }, []);

  return (
    <View style={styles.container}>
      <Text style={styles.title}>react-native-native-quickjs</Text>
      <Text style={styles.subtitle}>
        React Native runs on Hermes; the scripts below run in vendored QuickJS.
      </Text>
      <Button title="Run sync embedding demo" onPress={runDemo} />
      <Button title="Run worker + await demo" onPress={() => void runWorkerDemo()} />
      <ScrollView style={styles.output}>
        <Text selectable style={styles.outputText}>{report}</Text>
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    gap: 12,
    padding: 16,
    paddingTop: 48,
  },
  title: {
    fontSize: 20,
    fontWeight: '700',
  },
  subtitle: {
    color: '#555',
  },
  output: {
    flex: 1,
    backgroundColor: '#111',
    borderRadius: 8,
    padding: 12,
  },
  outputText: {
    color: '#72ff72',
    fontFamily: 'monospace',
  },
});
