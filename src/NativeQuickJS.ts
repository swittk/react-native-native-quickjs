import {NativeModules, TurboModuleRegistry} from 'react-native';
import type {TurboModule} from 'react-native';

/**
 * Registration-only module. The runtime API itself is installed through JSI.
 *
 * The synchronous install hook is intentionally part of the TurboModule spec
 * so React Native 0.73.6+ can use one JS-thread installation path without
 * depending on version-specific BindingsInstallerHolder protocols.
 */
export interface Spec extends TurboModule {
  installBindings(): void;
}

export type RegistrationModule = Spec;

const NativeQuickJS = (
  TurboModuleRegistry.get<Spec>('SKNativeQuickJS') ||
  (NativeModules.SKNativeQuickJS as Spec | undefined)
) as RegistrationModule | undefined;

export default NativeQuickJS;
