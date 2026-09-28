import {NativeModules, TurboModuleRegistry} from 'react-native';
import type {TurboModule} from 'react-native';

/** Registration-only module. The runtime API itself is installed through JSI. */
export interface Spec extends TurboModule {}

const NativeQuickJS =
  TurboModuleRegistry.get<Spec>('SKNativeQuickJS') ||
  (NativeModules.SKNativeQuickJS as Spec | undefined);

export default NativeQuickJS;
