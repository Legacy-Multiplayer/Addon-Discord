# Addon-Discord

A Discord bot inside Legacy Server, built on the shared [Addon-SDK](https://github.com/Legacy-Multiplayer/Addon-SDK). One native addon handles both directions: the persistent Gateway connection for incoming events and slash commands, and the REST API for messages, roles and moderation.

```lua
local discord = assert(createDiscordClient({ tokenFile = "discord.cfg" }))

addEventHandler("onDiscordMessageCreate", function(clientId, message)
    if clientId == discord and not message.author.bot then
        print("[Discord] " .. message.author.username .. ": " .. message.content)
    end
end)

registerDiscordCommand(discord, { name = "ping", description = "Check the bot" }, function(interaction)
    replyDiscordInteraction(discord, interaction, { content = "Pong!", ephemeral = true })
end)
```

- Gateway v10 with heartbeats, resume, reconnect backoff and clear errors for bad tokens or missing intents.
- REST v10 with a bounded queue, per-route waits and automatic 429 retries.
- Slash commands with Lua handlers, synced in bulk per guild or globally.
- Snowflake IDs stay strings; field names are camelCase on the Lua side.

See [docs/API.md](docs/API.md) for every function and event, [docs/BUILDING.md](docs/BUILDING.md) to compile, and [examples](examples/README.md) for a minimal bot.

Windows uses WinHTTP. Linux uses Boost.Beast and OpenSSL for HTTPS and WebSocket connections, with system certificate trust, hostname verification and TLS 1.2 or newer. Both platforms use the same Lua API.

## Get the source

```sh
git clone --recurse-submodules https://github.com/Legacy-Multiplayer/Addon-Discord.git
cd Addon-Discord
```

For an existing checkout, run `git submodule update --init --recursive`. The SDK is pinned as a Git submodule; rebuild against the exact SDK revision used by your server. GitHub source ZIP downloads do not include submodule contents.

## GitHub builds and releases

Pushes to `main` and pull requests build Windows x86 and Linux x86, run offline tests, and upload packages under **Actions → Build plugins → Artifacts**. Packages include the library, documentation, source/SDK revision manifest and a SHA-256 checksum. Windows produces `.dll`; Linux produces `.so`. A successful compilation does not prove live server behavior.

To prepare a release, manually run **Build plugins** on `main` with a new tag such as `v1.0.0`. Both builds must pass before a **draft** can be created. Nothing publishes automatically.

## Install

Extract the matching library into the stopped server's `addons` directory and enable it in the server configuration. Windows needs the x86 MSVC runtime. Linux needs compatible 32-bit C/C++ runtimes, OpenSSL 3 (`libssl.so.3` and `libcrypto.so.3`) and an up-to-date system CA certificate store; Boost is compiled into the addon and needs no runtime package.

Store the bot token in the local `resources/discord.cfg`, never in the repository. Follow [bot setup](docs/BUILDING.md#bot-setup), then load the [example](examples/README.md). The addon SDK fingerprint must match your server.

Offline tests and a token-free Discord connectivity probe exercise the transport. Bot authentication, permissions and live Lua behavior still require testing with your own application.
