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
  ' "$repo_dir")"
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
' "$repo_dir")}"

export REACT_NATIVE_GRADLE_PLUGIN_DIR="$react_plugin_dir"
export REACT_NATIVE_NODE_MODULES_DIR="$node_modules_dir"
export REACT_NATIVE_VERSION="$react_native_version"

"$gradle_command" --no-daemon --console=plain -p "$repo_dir/scripts"   -PnewArchEnabled=true   :react-native-native-quickjs-validation:compileDebugJavaWithJavac   :react-native-native-quickjs-validation:externalNativeBuildDebug

"$gradle_command" --no-daemon --console=plain -p "$repo_dir/scripts"   -PnewArchEnabled=false   :react-native-native-quickjs-validation:compileDebugJavaWithJavac   :react-native-native-quickjs-validation:externalNativeBuildDebug
