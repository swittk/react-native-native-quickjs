import {NativeModules, TurboModuleRegistry} from 'react-native';
import type {TurboModule} from 'react-native';

/** Registration-only module. The runtime API itself is installed through JSI. */
export interface Spec extends TurboModule {}

export type RegistrationModule = Spec & {
  /** Legacy-architecture synchronous JS-thread install hook. */
  installBindings?: () => boolean;
};

const NativeQuickJS = (
  TurboModuleRegistry.get<Spec>('SKNativeQuickJS') ||
  (NativeModules.SKNativeQuickJS as Spec | undefined)
) as RegistrationModule | undefined;

export default NativeQuickJS;
