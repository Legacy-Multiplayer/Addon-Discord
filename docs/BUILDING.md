# Building

Use CMake 3.20+, a C++17 compiler and a **32-bit** toolchain matching Legacy Server. Initialize the pinned SDK before configuring:

```sh
git submodule update --init --recursive
```

## Windows x86

Install Visual Studio Build Tools with x86 C++ and a Windows SDK:

```sh
cmake -S . -B build -A Win32
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --no-tests=error
python .github/scripts/package.py windows-x86
```

Output: `build/Release/Addon-Discord.dll`; package: `dist/Addon-Discord-windows-x86.zip`. The target machine requires the x86 MSVC runtime.

## Linux x86

On Ubuntu 22.04, install the compiler and multilib development libraries:

```sh
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install -y cmake gcc-multilib g++-multilib libboost-dev libssl-dev:i386 ca-certificates openssl python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32 \
  -DOPENSSL_SSL_LIBRARY=/usr/lib/i386-linux-gnu/libssl.so \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/i386-linux-gnu/libcrypto.so
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
python3 .github/scripts/package.py linux-x86
```

Output: `build/Addon-Discord.so`; package: `dist/Addon-Discord-linux-x86.tar.gz`. The target machine needs compatible 32-bit C/C++ runtimes, OpenSSL 3 libraries and CA certificates. On Ubuntu 22.04 enable the i386 architecture and install `libssl3:i386`, `libstdc++6:i386` and `ca-certificates`. Boost is header-only in this build; it is not a runtime dependency. The package is not a 64-bit plugin.

The packager requires a committed, clean checkout. For local packaging validation before the first commit, use `--allow-uncommitted`; its manifest explicitly records uncommitted/dirty source. Do not publish that validation package. A custom build directory can be supplied with `--build-dir`.

## SDK compatibility

The native addon API remains version 1. The exported SDK layout fingerprint must match the server exactly; equal version numbers alone do not establish compatibility. Do not edit the SDK copy inside this repository. Update its pinned submodule commit intentionally, then rebuild and validate.

## Installation after validation

Stop the server, extract the matching library into its `addons` directory, enable it in the server configuration, and restart. Check the addon initialization log and exercise the Lua API. GitHub Actions prepares downloadable artifacts and draft releases; it does not deploy to a running server.

## Networking and checks

Windows uses WinHTTP; Linux uses Boost 1.74+ headers and OpenSSL. TLS verifies the certificate chain and peer hostname, with TLS 1.2 minimum. All Linux native socket operations are serialized through their event loop. Request cancellation and bounded message queues keep networking independent of Lua callbacks; callbacks still run on the server thread.

Linux CTest includes local TLS fixtures for a trusted certificate, an untrusted certificate, wrong hostname, response/header parsing, oversized response and cancellation. The fixtures generate an ephemeral certificate with the OpenSSL command-line tool and contact only loopback.

Run `build/Release/LiveProbe.exe` on Windows or `./build/LiveProbe` on Linux for a token-free Discord connection check. It requests the public Gateway URL, receives Hello, sends a protocol heartbeat, checks its ACK, cancels a blocked read and reconnects. It sends no bot messages and does not authenticate. GitHub Actions also runs it on Linux with a 60-second deadline. External connectivity is required. Passing it does not validate bot permissions, authenticated session resume or live server Lua behavior.

## Bot setup

1. Create an application and Bot in the Discord Developer Portal.
2. Store the token locally in `resources/discord.cfg` as `token = "..."`. That filename and `.env` files are ignored by Git; never commit a real token or include one in release artifacts.
3. Enable Message Content for message contents, and Server Members if your selected intents require it.
4. Invite with `bot` and `applications.commands` scopes and only the permissions your Lua script needs.
5. See [API.md](API.md) for functions, events and request callbacks.
