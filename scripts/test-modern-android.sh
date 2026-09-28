#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
node_modules_dir="${REACT_NATIVE_NODE_MODULES_DIR:-$repo_dir/node_modules}"

if [[ ! -f "$node_modules_dir/react-native/package.json" ]]; then
  echo "React Native was not found under $node_modules_dir" >&2
  exit 2
fi

react_plugin_dir="${REACT_NATIVE_GRADLE_PLUGIN_DIR:-}"
if [[ -z "$react_plugin_dir" ]]; then
  react_plugin_dir="$(node -e '
    const path = require("path");
    const root = process.argv[1];
    const packageJson = require.resolve("@react-native/gradle-plugin/package.json", {paths: [root]});
    process.stdout.write(path.dirname(packageJson));
  ' "$node_modules_dir")"
fi
react_plugin_dir="$(cd "$react_plugin_dir" && pwd -P)"

gradle_command="${GRADLE_CMD:-$react_plugin_dir/gradlew}"
if [[ ! -x "$gradle_command" ]]; then
  echo "Modern Gradle wrapper is not executable: $gradle_command" >&2
  exit 2
fi

react_native_version="${REACT_NATIVE_VERSION:-$(node -e '
  const root = process.argv[1];
  process.stdout.write(require(require.resolve("react-native/package.json", {paths: [root]})).version);
' "$node_modules_dir")}"

android_gradle_plugin_version="${ANDROID_GRADLE_PLUGIN_VERSION:-$(
  sed -n 's/^agp *= *"\([^"]*\)".*/\1/p' \
    "$node_modules_dir/react-native/gradle/libs.versions.toml" | head -n 1
)}"
if [[ -z "$android_gradle_plugin_version" ]]; then
  echo "Unable to determine Android Gradle Plugin version from selected React Native tree" >&2
  exit 2
fi

export REACT_NATIVE_GRADLE_PLUGIN_DIR="$react_plugin_dir"
export REACT_NATIVE_NODE_MODULES_DIR="$node_modules_dir"
export REACT_NATIVE_VERSION="$react_native_version"
export ANDROID_GRADLE_PLUGIN_VERSION="$android_gradle_plugin_version"

"$gradle_command" --no-daemon --console=plain -p "$repo_dir/scripts"   -PnewArchEnabled=true   :react-native-native-quickjs-validation:compileDebugJavaWithJavac   :react-native-native-quickjs-validation:externalNativeBuildDebug

"$gradle_command" --no-daemon --console=plain -p "$repo_dir/scripts"   -PnewArchEnabled=false   :react-native-native-quickjs-validation:compileDebugJavaWithJavac   :react-native-native-quickjs-validation:externalNativeBuildDebug
