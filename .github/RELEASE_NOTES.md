# Addon-Discord v1.0.0

Native Discord Gateway and REST bridge for Legacy Multiplayer servers, with Lua events, slash commands and asynchronous requests.

## Downloads

Windows x86 `.dll` and Linux x86 `.so` packages include documentation, a source/SDK revision manifest and SHA-256 checksums. The addon must match the server SDK fingerprint. Windows requires the x86 MSVC runtime; Linux uses an Ubuntu 22.04 x86 build baseline with compatible 32-bit runtimes.

## Networking

Both platforms provide HTTPS REST and persistent WebSocket Gateway connections. Windows uses WinHTTP; Linux uses Boost.Beast with OpenSSL, certificate/hostname verification and cancellable asynchronous I/O. Linux requires OpenSSL 3 and an up-to-date CA certificate store in addition to its 32-bit C/C++ runtimes.

The Lua API is the same on both platforms. Transport tests include offline checks and a token-free Gateway probe. Test your bot token, intents, permissions and server-script callbacks before production use.

See the repository README and docs for installation and API details. Clone recursively when building from source.
