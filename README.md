<div align="center">
  <h1><code>StarlingMonkey</code></h1>

  <p>
    <strong>A SpiderMonkey-based JS runtime on WebAssembly</strong>
  </p>

<strong>A <a href="https://bytecodealliance.org/">Bytecode Alliance</a> project</strong>

  <p>
    <a href="https://github.com/bytecodealliance/StarlingMonkey/actions?query=workflow%3ACI"><img src="https://github.com/bytecodealliance/StarlingMonkey/workflows/CI/badge.svg" alt="build status" /></a>
    <a href="https://bytecodealliance.zulipchat.com/#narrow/stream/459697-StarlingMonkey"><img src="https://img.shields.io/badge/zulip-join_chat-brightgreen.svg" alt="zulip chat" /></a>
  </p>

  <h3>
    <a href="#quick-start">Building</a>
    <span> | </span>
    <a href="ADOPTERS.md">Adopters</a>
    <span> | </span>
    <a href="https://bytecodealliance.github.io/StarlingMonkey">Documentation</a>
    <span> | </span>
    <a href="https://bytecodealliance.zulipchat.com/#narrow/stream/459697-StarlingMonkey">Chat</a>
  </h3>
</div>

StarlingMonkey is a [SpiderMonkey][spidermonkey] based JS runtime optimized for use in [WebAssembly
Components][wasm-component]. StarlingMonkey's core builtins target WASI 0.2.0 to support a Component
Model based event loop and standards-compliant implementations of key web builtins, including the
fetch API, WHATWG Streams, text encoding, and others. To support tailoring for specific use cases,
it's designed to be highly modular, and can be readily extended with custom builtins and host APIs.

StarlingMonkey is used in production for Fastly's JS Compute platform, and Fermyon's Spin JS SDK.
See the [ADOPTERS](ADOPTERS.md) file for more details.

## Documentation

For comprehensive documentation, visit our [Documentation Site][gh-pages].

## Quick Start

### Requirements

The runtime's build is managed by [cmake][cmake], which also takes care of downloading the build
dependencies. To properly manage the Rust toolchain, the build script expects
[rustup](https://rustup.rs/) to be installed in the system.

### Usage

With sufficiently new versions of `cmake` and `rustup` installed, the build process is as follows:

1. Clone the repo

```bash
$ git clone https://github.com/bytecodealliance/StarlingMonkey
$ cd StarlingMonkey
```

2. Run the configuration script

For a release configuration, run

```bash
$ cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
```

For a debug configuration, run

```bash
$ cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
```

3. Build the runtime

Building the runtime is done in two phases: first, cmake is used to build a raw version as a
WebAssembly core module. Then, that module is turned into a [WebAssembly Component][wasm-component]
using the `componentize.sh` script.

The following command will build the `starling-raw.wasm` runtime module in the `cmake-build-release`
directory:

```bash
# Use cmake-build-debug for the debug build
# Change the value for `--parallel` to match the number of CPU cores in your system
$ cmake --build cmake-build-release --parallel 8
```

Then, the `starling-raw.wasm` module can be turned into a component with the following command:

```bash
$ cd cmake-build-release
$ ./componentize.sh -o starling.wasm
```

The resulting runtime can be used to load and evaluate JS code dynamically:

```bash
$ wasmtime -S http starling.wasm -e "console.log('hello world')"
# or, to load a file:
$ wasmtime -S http --dir . starling.wasm index.js
```

Alternatively, a JS file can be provided during componentization:

```bash
$ cd cmake-build-release
$ ./componentize.sh index.js -o starling.wasm
```

This way, the JS file will be loaded during componentization, and the top-level code will be
executed, and can e.g. register a handler for the `fetch` event to serve HTTP requests.

[cmake]: https://cmake.org/
[gh-pages]: https://bytecodealliance.github.io/StarlingMonkey/
[spidermonkey]: https://spidermonkey.dev/
[wasm-component]: https://component-model.bytecodealliance.org/
