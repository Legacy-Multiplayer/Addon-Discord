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

The transport uses WinHTTP, so networking currently works on Windows only. The Linux build compiles with a stub transport that reports an error instead of connecting.

## Get the source

```sh
git clone --recurse-submodules https://github.com/Legacy-Multiplayer/Addon-Discord.git
cd Addon-Discord
```

For an existing checkout, run `git submodule update --init --recursive`. The SDK is pinned as a Git submodule; rebuild against the exact SDK revision used by your server. GitHub source ZIP downloads do not include submodule contents.

## GitHub builds and releases

Pushes to `main` and pull requests build Windows x86 and Linux x86, run offline tests, and upload packages under **Actions → Build plugins → Artifacts**. Packages include the library, documentation, source/SDK revision manifest and a SHA-256 checksum. Windows produces `.dll`; Linux produces `.so`. A successful compilation does not prove live server behavior.

To prepare a release, manually run **Build plugins** on `main` with a new tag such as `v1.0.0`. Both builds must pass before a **draft** can be created. Nothing publishes automatically.

**Release blocker:** Linux currently uses a nonfunctional transport stub. Its Actions artifact is for build validation only. Draft-release creation is deliberately blocked until Linux networking is implemented and validated. Do not distribute that `.so` as a working Discord connector.
