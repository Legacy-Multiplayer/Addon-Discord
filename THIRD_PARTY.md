# Third-party components

## Boost.Asio and Boost.Beast (Linux)

The Linux transport uses Boost headers, available under the [Boost Software License 1.0](https://www.boost.org/LICENSE_1_0.txt). Copyright belongs to the respective Boost contributors, including Christopher M. Kohlhoff (Asio) and Vinnie Falco (Beast). Headers are obtained from the build system rather than copied into this repository. Header code is compiled into the library; no Boost runtime library is shipped.

## OpenSSL (Linux)

The addon dynamically links the system OpenSSL libraries. CI targets Ubuntu 22.04's OpenSSL 3 packages; [OpenSSL 3 is licensed under Apache 2.0](https://www.openssl.org/source/license.html). OpenSSL binaries are not bundled in the release package. Install compatible system libraries and keep their security updates and CA certificates current.

## WinHTTP (Windows)

Windows networking uses the operating system's WinHTTP API. No Windows system library is redistributed.

## Addon-SDK

The shared [Addon-SDK](https://github.com/Legacy-Multiplayer/Addon-SDK) is pinned as a Git submodule and carries its own MIT license. Recursive clones include that license in the SDK folder.
