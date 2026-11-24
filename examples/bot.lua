-- Configure resources/discord.cfg locally; never put a bot token in this file.
local bot

addEventHandler("onDiscordReady", function(clientId, user)
    if clientId == bot then
        print("Discord bot connected: " .. (user.username or "unknown"))
    end
end)

addEventHandler("onDiscordError", function(clientId, message)
    if clientId == bot then
        print("Discord error: " .. message)
    end
end)

addEventHandler("onMainScriptInit", function()
    local err
    bot, err = createDiscordClient({ tokenFile = "discord.cfg", intents = {"GUILDS"} })
    assert(bot, err)

    registerDiscordCommand(bot, {
        name = "ping",
        description = "Check the game server connection",
    }, function(interaction)
        replyDiscordInteraction(bot, interaction, {
            content = "The game server is online.",
            ephemeral = true,
        }, function(ok, _, errorMessage)
            if not ok then print("Discord reply failed: " .. errorMessage) end
        end)
    end)
end)

-- The addon resets its sessions on main-script shutdown and unload.
