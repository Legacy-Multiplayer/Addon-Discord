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
sudo apt-get update
sudo apt-get install -y cmake gcc-multilib g++-multilib
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
python3 .github/scripts/package.py linux-x86
```

Output: `build/Addon-Discord.so`; package: `dist/Addon-Discord-linux-x86.tar.gz`. The target machine needs compatible 32-bit C/C++ runtimes. The package is not a 64-bit plugin.

The packager requires a committed, clean checkout. For local packaging validation before the first commit, use `--allow-uncommitted`; its manifest explicitly records uncommitted/dirty source. Do not publish that validation package. A custom build directory can be supplied with `--build-dir`.

## SDK compatibility

The native addon API remains version 1. The exported SDK layout fingerprint must match the server exactly; equal version numbers alone do not establish compatibility. Do not edit the SDK copy inside this repository. Update its pinned submodule commit intentionally, then rebuild and validate.

## Installation after validation

Stop the server, extract the matching library into its `addons` directory, enable it in the server configuration, and restart. Check the addon initialization log and exercise the Lua API. GitHub Actions prepares downloadable artifacts and draft releases; it does not deploy to a running server.

## Current platform limitation

Windows uses system WinHTTP. Linux currently compiles a transport stub which returns an unsupported-transport error. A Linux build checks portability of the remaining source only; it cannot run a Discord bot. The manual draft-release job is blocked by `.github/release.json`. Do not mark it ready merely to make CI green.

## Optional Windows connectivity check

Run `build/Release/LiveProbe.exe` manually. It requests Discord's public Gateway URL and waits for an anonymous WebSocket Hello. It needs no bot token and sends no bot messages. It is excluded from offline CTest runs because it requires external network access. Passing it does not validate authentication, command registration, reconnect/resume or bot permissions.

## Bot setup

1. Create an application and Bot in the Discord Developer Portal.
2. Store the token locally in `resources/discord.cfg` as `token = "..."`. That filename and `.env` files are ignored by Git; never commit a real token or include one in release artifacts.
3. Enable Message Content for message contents, and Server Members if your selected intents require it.
4. Invite with `bot` and `applications.commands` scopes and only the permissions your Lua script needs.
5. See [API.md](API.md) for functions, events and request callbacks.
