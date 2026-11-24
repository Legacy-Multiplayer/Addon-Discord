# Minimal bot example

Load `bot.lua` from your main script before `onMainScriptInit`. It starts a client, logs connection errors and registers `/ping`, which responds privately. No Lua adapter is required.

Create a bot in Discord's Developer Portal. Invite it with `bot` and `applications.commands` scopes. Put its token in the local server file `resources/discord.cfg`:

```text
token = "YOUR_BOT_TOKEN"
```

Never commit that file. The example requests only the GUILDS intent; it does not read message contents or need the privileged Message Content intent. Without `guildId` the command is global; add your test guild ID to its definition for guild-scoped testing. Command syncing replaces commands in the registered scope, so use a dedicated test application when trying the example.

This example is for the Windows transport. Linux networking is currently a stub and cannot connect. No live bot messages were sent during local validation.
