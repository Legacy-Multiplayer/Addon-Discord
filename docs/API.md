# Discord functions

Load the native Addon-Discord plugin by placing `Addon-Discord.dll` in the server's `addons` folder. These are global Lua functions; no Lua file is required. Networking runs on background threads; every callback and event arrives on the server thread.

Discord IDs are always **strings**. Field names are converted to camelCase in both directions: Discord's `channel_id` arrives as `channelId`, and you may send `allowedMentions`, which is sent as `allowed_mentions`.

## Connection

### createDiscordClient(options)

| Option | Meaning |
| --- | --- |
| `token` | Bot token. Prefer `tokenFile` so the token never sits in a script. |
| `tokenFile` | File under `resources` with a `token = "..."` line. Default `discord.cfg`. |
| `intents` | Array of names (`"GUILDS"`, `"GUILD_MESSAGES"`, `"MESSAGE_CONTENT"`, `"GUILD_MEMBERS"`, `"GUILD_MESSAGE_REACTIONS"`, ...) or numbers, or one integer bitmask. Default: GUILDS, GUILD_MESSAGES, MESSAGE_CONTENT. |
| `presence` | `{status = "online", activity = {name = "...", type = 0}}` |

Return: `clientId, ""` or `nil, errorMessage`. The connection continues in the background; wait for `onDiscordReady`.

`MESSAGE_CONTENT` and `GUILD_MEMBERS` are privileged: enable them under **Bot → Privileged Gateway Intents** in the Developer Portal, or the gateway closes with code 4014.

### destroyDiscordClient(clientId), isDiscordClientConnected(clientId), getDiscordUser(clientId)

Stop a client, check its connection, or read the bot's own user table (nil before ready).

## Requests

Every request function returns `requestId, ""` when queued or `false, errorMessage` when the queue (256) is full. Invalid IDs or payloads raise a Lua error. Optional callbacks receive `callback(true, result)` or `callback(false, nil, errorMessage)`. Without a callback, failures are reported through `onDiscordError`. Rate limits and 429 retries are handled automatically.

A message `payload` is either a string (the content) or a table such as `{content = "...", embeds = {...}, components = {...}, allowedMentions = {parse = {}}}`. In interaction replies, `ephemeral = true` makes the reply visible only to the user.

| Function | Discord action |
| --- | --- |
| `sendDiscordMessage(client, channelId, payload, callback?)` | Post a message |
| `editDiscordMessage(client, channelId, messageId, payload, callback?)` | Edit a bot message |
| `deleteDiscordMessage(client, channelId, messageId, callback?)` | Delete a message |
| `getDiscordMessage(client, channelId, messageId, callback)` | Fetch a message |
| `getDiscordChannel(client, channelId, callback)` | Fetch a channel |
| `getDiscordMember(client, guildId, userId, callback)` | Fetch a guild member |
| `addDiscordMemberRole(client, guildId, userId, roleId, callback?)` | Give a role |
| `removeDiscordMemberRole(client, guildId, userId, roleId, callback?)` | Remove a role |
| `timeoutDiscordMember(client, guildId, userId, seconds, reason?, callback?)` | Timeout (0 removes it; max 28 days) |
| `kickDiscordMember(client, guildId, userId, reason?, callback?)` | Kick |
| `banDiscordMember(client, guildId, userId, reason?, deleteMessageSeconds?, callback?)` | Ban |
| `discordRequest(client, method, path, body?, callback?)` | Any other endpoint under `/api/v10` |

## Slash commands

### registerDiscordCommand(client, definition, handler?)

```lua
registerDiscordCommand(discord, {
    name = "kick",
    description = "Kick a player from the game",
    guildId = "123456789012345678",
    options = {
        { name = "player", description = "Name or ID", type = "string", required = true },
        { name = "reason", description = "Why", type = "string" },
    },
}, function(interaction, options)
    replyDiscordInteraction(discord, interaction, { content = "Kicking " .. options.player, ephemeral = true })
end)
```

Option types: `string`, `integer`, `number`, `boolean`, `user`, `channel`, `role`, `mentionable`, `attachment`, `subcommand`, `group`. The handler gets the full interaction and a flat `options` table (`options.subcommand` and `options.group` for nested commands). A `guildId` command appears immediately in that server; without it the command is global and Discord may take up to an hour to show it. Registrations are synced in bulk shortly after the bot is ready, so each scope always matches what the script registered. Discord expects a reply within 3 seconds; use `deferDiscordInteraction` for slow work.

### removeDiscordCommand(client, name, guildId?)

### replyDiscordInteraction(client, interaction, payload, callback?)
### deferDiscordInteraction(client, interaction, ephemeral?, callback?)
### editDiscordInteractionReply(client, interaction, payload, callback?)

### setDiscordPresence(client, {status, activity = {name, type, state}})

Activity types: 0 playing, 1 streaming, 2 listening, 3 watching, 4 custom (uses `state`), 5 competing.

## Events

Every handler receives the client ID first.

| Event | Second argument |
| --- | --- |
| `onDiscordReady` | Bot user table |
| `onDiscordDisconnected` | `{code, reason, reconnecting, wasConnected}` |
| `onDiscordError` | Error message string |
| `onDiscordMessageCreate`, `onDiscordMessageUpdate` | Message table |
| `onDiscordMessageDelete` | `{id, channelId, guildId}` |
| `onDiscordInteraction` | Interaction table (also fires for registered commands) |
| `onDiscordMemberJoin`, `onDiscordMemberLeave` | Member data (needs `GUILD_MEMBERS`) |
| `onDiscordReactionAdd` | Reaction data (needs `GUILD_MESSAGE_REACTIONS`) |

The gateway reconnects and resumes automatically. Authentication and intent errors (close codes 4004 and 4014) stop the client and are explained in `onDiscordError`.
