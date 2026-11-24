#include "SDK/Plugin.h"
#include "Client.h"
#include "Json.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace SDK;

namespace
{
Json::Value ToJson(const CValue& value, int depth = 0);

Json::Value TableToJson(const CTable& table, int depth)
{
    if (depth > 32)
    {
        throw std::invalid_argument("Discord payload nesting is too deep");
    }

    const auto& entries = table.GetEntries();
    bool array = true;
    int64_t expected = 1;

    for (const auto& entry : entries)
    {
        const auto* index = std::get_if<int64_t>(&entry.first);

        if (!index || *index != expected)
        {
            array = false;
            break;
        }

        ++expected;
    }

    if (array)
    {
        Json::Array items;

        for (const auto& entry : entries)
        {
            items.push_back(ToJson(entry.second, depth + 1));
        }

        return Json::Value(std::move(items));
    }

    Json::Object object;

    for (const auto& entry : entries)
    {
        std::string key;

        if (const auto* text = std::get_if<std::string>(&entry.first))
        {
            key = *text;
        }
        else
        {
            key = std::to_string(std::get<int64_t>(entry.first));
        }

        object.emplace_back(key, ToJson(entry.second, depth + 1));
    }

    return Json::Value(std::move(object));
}

Json::Value ToJson(const CValue& value, int depth)
{
    return std::visit([depth](const auto& item) -> Json::Value
    {
        using Type = std::decay_t<decltype(item)>;

        if constexpr (std::is_same_v<Type, std::monostate>)
        {
            return Json::Value();
        }
        else if constexpr (std::is_same_v<Type, std::shared_ptr<const CTable>>)
        {
            return item ? TableToJson(*item, depth) : Json::Value();
        }
        else
        {
            return Json::Value(item);
        }
    }, value.GetData());
}

CValue ToLua(const Json::Value& value, int depth = 0)
{
    if (depth > 14)
    {
        return CValue{};
    }

    return std::visit([depth](const auto& item) -> CValue
    {
        using Type = std::decay_t<decltype(item)>;

        if constexpr (std::is_same_v<Type, std::monostate>)
        {
            return CValue{};
        }
        else if constexpr (std::is_same_v<Type, std::shared_ptr<Json::Array>>)
        {
            CTable table;
            int64_t index = 1;

            for (const auto& entry : *item)
            {
                if (index > 1024)
                {
                    break;
                }

                table.Set(index++, ToLua(entry, depth + 1));
            }

            return CValue(table);
        }
        else if constexpr (std::is_same_v<Type, std::shared_ptr<Json::Object>>)
        {
            CTable table;

            for (const auto& entry : *item)
            {
                if (!entry.second.IsNull())
                {
                    table.Set(entry.first, ToLua(entry.second, depth + 1));
                }
            }

            return CValue(table);
        }
        else
        {
            return CValue(item);
        }
    }, value.GetData());
}

Json::Value MessagePayload(const CValue& payload)
{
    if (const auto* text = std::get_if<std::string>(&payload.GetData()))
    {
        Json::Value message;
        message.Set("content", *text);
        return message;
    }

    if (!std::holds_alternative<std::shared_ptr<const CTable>>(payload.GetData()))
    {
        throw std::invalid_argument("Message payload must be a string or table");
    }

    auto message = Json::RenameKeys(ToJson(payload), false);

    if (!message.IsObject())
    {
        throw std::invalid_argument("Message payload table must have named fields");
    }

    if (const auto* ephemeral = message.Find("ephemeral"))
    {
        const bool hidden = ephemeral->Bool(false);
        message.Remove("ephemeral");

        if (hidden)
        {
            message.Set("flags", message["flags"].Integer(0) | 64);
        }
    }

    return message;
}

void RequireSnowflake(const std::string& id, const char* what)
{
    if (id.empty() || id.size() > 25 || id.find_first_not_of("0123456789") != std::string::npos)
    {
        throw std::invalid_argument(std::string("Invalid Discord ") + what + " ID (IDs are strings of digits)");
    }
}

int64_t OptionType(const Json::Value& type)
{
    if (type.IsNumber())
    {
        return type.Integer();
    }

    static const std::map<std::string, int64_t> names = {
        {"subcommand", 1},
        {"group", 2},
        {"string", 3},
        {"integer", 4},
        {"boolean", 5},
        {"user", 6},
        {"channel", 7},
        {"role", 8},
        {"mentionable", 9},
        {"number", 10},
        {"attachment", 11},
    };

    std::string key = type.String("string");
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    const auto found = names.find(key);

    if (found == names.end())
    {
        throw std::invalid_argument("Unknown command option type: " + key);
    }

    return found->second;
}

Json::Value NormalizeOptions(const Json::Value& options)
{
    Json::Array output;

    for (const auto& option : options.Items())
    {
        auto copy = option;
        copy.Set("type", OptionType(option["type"]));

        if (const auto* nested = option.Find("options"))
        {
            copy.Set("options", NormalizeOptions(*nested));
        }

        output.push_back(std::move(copy));
    }

    return Json::Value(std::move(output));
}

void FlattenOptions(const Json::Value& options, CTable& output)
{
    for (const auto& option : options.Items())
    {
        const auto type = option["type"].Integer();
        const auto name = option["name"].String();

        if (type == 1)
        {
            output.Set("subcommand", name);
            FlattenOptions(option["options"], output);
        }
        else if (type == 2)
        {
            output.Set("group", name);
            FlattenOptions(option["options"], output);
        }
        else if (const auto* value = option.Find("value"))
        {
            output.Set(name, ToLua(*value));
        }
    }
}

std::string ReadToken(const std::filesystem::path& root, const std::string& file)
{
    if (file.empty() || file.find("..") != std::string::npos || std::filesystem::path(file).is_absolute())
    {
        throw std::invalid_argument("tokenFile must be a relative path inside resources");
    }

    std::ifstream input(root / file);

    if (!input)
    {
        throw std::runtime_error("Cannot open token file resources/" + file);
    }

    std::string line;

    while (std::getline(input, line))
    {
        const auto first = line.find_first_not_of(" \t\r");

        if (first == std::string::npos || line[first] == '#' || line[first] == ';')
        {
            continue;
        }

        const auto equals = line.find('=');

        if (equals == std::string::npos)
        {
            continue;
        }

        auto key = line.substr(first, equals - first);
        key.erase(key.find_last_not_of(" \t") + 1);

        if (key != "token")
        {
            continue;
        }

        auto value = line.substr(equals + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r") + 1);

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1, value.size() - 2);
        }

        return value;
    }

    throw std::runtime_error("No token = \"...\" line in resources/" + file);
}

} // namespace

class CDiscord final : public CPlugin
{
    struct Command
    {
        Json::Value definition;
        std::optional<CCallback> handler;
    };

    struct Session
    {
        std::set<uint64_t> commandSyncs;
        std::unique_ptr<Discord::Client> client;
        std::map<uint64_t, CCallback> callbacks;
        std::map<std::string, std::map<std::string, Command>> commands;
        std::set<std::string> dirtyScopes;
        std::chrono::steady_clock::time_point syncAt;
        bool ready = false;
    };

    bool OnLoad() override
    {
        m_root = std::filesystem::current_path() / "resources";

        Function("createDiscordClient", [this](CTable options)
        {
            try
            {
                std::string token = options.Get<std::string>("token", std::string());

                if (token.empty())
                {
                    token = ReadToken(m_root, options.Get<std::string>("tokenFile", std::string("discord.cfg")));
                }

                if (token.size() < 20 || token.find_first_of(" \r\n\t") != std::string::npos)
                {
                    throw std::invalid_argument("The bot token looks invalid");
                }

                int64_t intents = (1 << 0) | (1 << 9) | (1 << 15);

                if (options.Contains("intents"))
                {
                    intents = 0;
                    const auto value = options.Get<CValue>("intents");

                    if (const auto* number = std::get_if<int64_t>(&value.GetData()))
                    {
                        intents = *number;
                    }
                    else
                    {
                        const auto list = value.As<CTable>();

                        for (const auto& entry : list.GetEntries())
                        {
                            if (const auto* bits = std::get_if<int64_t>(&entry.second.GetData()))
                            {
                                intents |= *bits;
                            }
                            else
                            {
                                const auto name = entry.second.As<std::string>();
                                const auto bit = Discord::IntentValue(name);

                                if (bit < 0)
                                {
                                    throw std::invalid_argument("Unknown intent: " + name);
                                }

                                intents |= bit;
                            }
                        }
                    }
                }

                Json::Value presence;

                if (options.Contains("presence"))
                {
                    presence = PresencePayload(options.Get<CTable>("presence"));
                }

                if (m_sessions.size() >= 4)
                {
                    throw std::runtime_error("At most 4 Discord clients may run at once");
                }

                const int64_t id = ++m_nextClient;
                auto session = std::make_unique<Session>();
                session->client = std::make_unique<Discord::Client>(token, intents, presence);
                session->client->Start();
                m_sessions.emplace(id, std::move(session));
                return std::make_tuple(std::optional<int64_t>(id), std::string());
            }
            catch (const std::exception& error)
            {
                return std::make_tuple(std::optional<int64_t>{}, std::string(error.what()));
            }
        });

        Function("destroyDiscordClient", [this](int64_t id)
        {
            const auto found = m_sessions.find(id);

            if (found == m_sessions.end())
            {
                return false;
            }

            found->second->client->Stop();
            m_sessions.erase(found);
            return true;
        });

        Function("isDiscordClientConnected", [this](int64_t id)
        {
            const auto found = m_sessions.find(id);
            return found != m_sessions.end() && found->second->client->Connected();
        });

        Function("getDiscordUser", [this](int64_t id)
        {
            auto& session = Get(id);
            const auto user = session.client->User();
            return user.IsObject() ? ToLua(Json::RenameKeys(user, true)) : CValue{};
        });

        Function("sendDiscordMessage", [this](int64_t id, std::string channelId, CValue payload, std::optional<CCallback> callback)
        {
            RequireSnowflake(channelId, "channel");
            const auto body = MessagePayload(payload);
            return Queue(id, "POST", "/channels/" + channelId + "/messages", &body, {}, std::move(callback));
        });

        Function("editDiscordMessage", [this](int64_t id, std::string channelId, std::string messageId, CValue payload, std::optional<CCallback> callback)
        {
            RequireSnowflake(channelId, "channel");
            RequireSnowflake(messageId, "message");
            const auto body = MessagePayload(payload);
            return Queue(id, "PATCH", "/channels/" + channelId + "/messages/" + messageId, &body, {}, std::move(callback));
        });

        Function("deleteDiscordMessage", [this](int64_t id, std::string channelId, std::string messageId, std::optional<CCallback> callback)
        {
            RequireSnowflake(channelId, "channel");
            RequireSnowflake(messageId, "message");
            return Queue(id, "DELETE", "/channels/" + channelId + "/messages/" + messageId, nullptr, {}, std::move(callback));
        });

        Function("getDiscordMessage", [this](int64_t id, std::string channelId, std::string messageId, CCallback callback)
        {
            RequireSnowflake(channelId, "channel");
            RequireSnowflake(messageId, "message");
            return Queue(id, "GET", "/channels/" + channelId + "/messages/" + messageId, nullptr, {}, std::move(callback));
        });

        Function("getDiscordChannel", [this](int64_t id, std::string channelId, CCallback callback)
        {
            RequireSnowflake(channelId, "channel");
            return Queue(id, "GET", "/channels/" + channelId, nullptr, {}, std::move(callback));
        });

        Function("getDiscordMember", [this](int64_t id, std::string guildId, std::string userId, CCallback callback)
        {
            RequireSnowflake(guildId, "guild");
            RequireSnowflake(userId, "user");
            return Queue(id, "GET", "/guilds/" + guildId + "/members/" + userId, nullptr, {}, std::move(callback));
        });

        Function("addDiscordMemberRole", [this](int64_t id, std::string guildId, std::string userId, std::string roleId, std::optional<CCallback> callback)
        {
            RequireSnowflake(guildId, "guild");
            RequireSnowflake(userId, "user");
            RequireSnowflake(roleId, "role");
            return Queue(id, "PUT", "/guilds/" + guildId + "/members/" + userId + "/roles/" + roleId, nullptr, {}, std::move(callback));
        });

        Function("removeDiscordMemberRole", [this](int64_t id, std::string guildId, std::string userId, std::string roleId, std::optional<CCallback> callback)
        {
            RequireSnowflake(guildId, "guild");
            RequireSnowflake(userId, "user");
            RequireSnowflake(roleId, "role");
            return Queue(id, "DELETE", "/guilds/" + guildId + "/members/" + userId + "/roles/" + roleId, nullptr, {}, std::move(callback));
        });

        Function("timeoutDiscordMember", [this](int64_t id, std::string guildId, std::string userId, int64_t seconds, std::optional<std::string> reason, std::optional<CCallback> callback)
        {
            RequireSnowflake(guildId, "guild");
            RequireSnowflake(userId, "user");

            if (seconds < 0 || seconds > 28 * 24 * 3600)
            {
                throw std::invalid_argument("Timeout must be 0 to 2419200 seconds (0 removes it)");
            }

            Json::Value body;
            body.Set("communication_disabled_until", seconds == 0 ? Json::Value() : Json::Value(IsoTimeIn(seconds)));
            return Queue(id, "PATCH", "/guilds/" + guildId + "/members/" + userId, &body, reason.value_or(""), std::move(callback));
        });

        Function("kickDiscordMember", [this](int64_t id, std::string guildId, std::string userId, std::optional<std::string> reason, std::optional<CCallback> callback)
        {
            RequireSnowflake(guildId, "guild");
            RequireSnowflake(userId, "user");
            return Queue(id, "DELETE", "/guilds/" + guildId + "/members/" + userId, nullptr, reason.value_or(""), std::move(callback));
        });

        Function("banDiscordMember", [this](int64_t id, std::string guildId, std::string userId, std::optional<std::string> reason, std::optional<int64_t> deleteMessageSeconds, std::optional<CCallback> callback)
        {
            RequireSnowflake(guildId, "guild");
            RequireSnowflake(userId, "user");
            Json::Value body;
            body.Set("delete_message_seconds", std::clamp<int64_t>(deleteMessageSeconds.value_or(0), 0, 604800));
            return Queue(id, "PUT", "/guilds/" + guildId + "/bans/" + userId, &body, reason.value_or(""), std::move(callback));
        });

        Function("registerDiscordCommand", [this](int64_t id, CTable definition, std::optional<CCallback> handler)
        {
            auto& session = Get(id);
            auto json = Json::RenameKeys(TableToJson(definition, 0), false);
            const auto name = json["name"].String();

            if (name.empty() || name.size() > 32 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
            {
                throw std::invalid_argument("Command names must be 1-32 lowercase letters, digits, - or _");
            }

            if (json["description"].String().empty())
            {
                json.Set("description", "No description");
            }

            const auto scope = json["guild_id"].String();

            if (!scope.empty())
            {
                RequireSnowflake(scope, "guild");
            }

            json.Remove("guild_id");

            if (const auto* options = json.Find("options"))
            {
                json.Set("options", NormalizeOptions(*options));
            }

            json.Set("type", 1);
            auto& commands = session.commands[scope];

            if (commands.size() >= 100 && !commands.count(name))
            {
                throw std::length_error("Discord allows at most 100 commands per scope");
            }

            commands[name] = Command{json, std::move(handler)};
            MarkDirty(session, scope);
            return true;
        });

        Function("removeDiscordCommand", [this](int64_t id, std::string name, std::optional<std::string> guildId)
        {
            auto& session = Get(id);
            const auto scope = guildId.value_or("");
            auto found = session.commands.find(scope);

            if (found == session.commands.end() || !found->second.erase(name))
            {
                return false;
            }

            MarkDirty(session, scope);
            return true;
        });

        Function("replyDiscordInteraction", [this](int64_t id, CTable interaction, CValue payload, std::optional<CCallback> callback)
        {
            Json::Value body;
            body.Set("type", 4);
            body.Set("data", MessagePayload(payload));
            return Queue(id, "POST", InteractionPath(interaction) + "/callback", &body, {}, std::move(callback));
        });

        Function("deferDiscordInteraction", [this](int64_t id, CTable interaction, std::optional<bool> ephemeral, std::optional<CCallback> callback)
        {
            Json::Value body;
            body.Set("type", 5);

            if (ephemeral.value_or(false))
            {
                Json::Value data;
                data.Set("flags", 64);
                body.Set("data", data);
            }

            return Queue(id, "POST", InteractionPath(interaction) + "/callback", &body, {}, std::move(callback));
        });

        Function("editDiscordInteractionReply", [this](int64_t id, CTable interaction, CValue payload, std::optional<CCallback> callback)
        {
            auto& session = Get(id);
            const auto application = session.client->ApplicationId();

            if (application.empty())
            {
                throw std::runtime_error("The Discord client is not ready yet");
            }

            const auto token = interaction.Get<std::string>("token", std::string());

            if (token.empty() || token.size() > 512)
            {
                throw std::invalid_argument("Interaction table has no token");
            }

            const auto body = MessagePayload(payload);
            return Queue(id, "PATCH", "/webhooks/" + application + "/" + token + "/messages/@original", &body, {}, std::move(callback));
        });

        Function("setDiscordPresence", [this](int64_t id, CTable presence)
        {
            Get(id).client->UpdatePresence(PresencePayload(presence));
            return true;
        });

        Function("discordRequest", [this](int64_t id, std::string method, std::string path, std::optional<CValue> body, std::optional<CCallback> callback)
        {
            static const std::set<std::string> methods = {"GET", "POST", "PUT", "PATCH", "DELETE"};

            if (!methods.count(method))
            {
                throw std::invalid_argument("Method must be GET, POST, PUT, PATCH or DELETE");
            }

            if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos || path.find_first_of(" \r\n") != std::string::npos)
            {
                throw std::invalid_argument("Path must start with / and be relative to /api/v10");
            }

            if (body && !body->IsNil())
            {
                const auto json = Json::RenameKeys(ToJson(*body), false);
                return Queue(id, method, path, &json, {}, std::move(callback));
            }

            return Queue(id, method, path, nullptr, {}, std::move(callback));
        });

        Every(25, [this]()
        {
            Pump();
        });

        Event("onMainScriptExit", [this]()
        {
            Reset();
        });

        return true;
    }

    void OnUnload() override
    {
        Reset();
    }

    void Reset()
    {
        for (auto& entry : m_sessions)
        {
            entry.second->client->Stop();
        }

        m_sessions.clear();
    }

    Session& Get(int64_t id)
    {
        const auto found = m_sessions.find(id);

        if (found == m_sessions.end())
        {
            throw std::invalid_argument("Unknown Discord client ID");
        }

        return *found->second;
    }

    std::tuple<CValue, std::string> Queue(int64_t id, const std::string& method, const std::string& path, const Json::Value* body,
        const std::string& reason, std::optional<CCallback> callback)
    {
        auto& session = Get(id);
        const auto request = session.client->Enqueue(method, path, body, reason);

        if (!request)
        {
            return {CValue(false), "Discord request queue is full"};
        }

        if (callback)
        {
            session.callbacks.emplace(request, std::move(*callback));
        }

        return {CValue(static_cast<int64_t>(request)), std::string()};
    }

    static std::string InteractionPath(const CTable& interaction)
    {
        const auto id = interaction.Get<std::string>("id", std::string());
        const auto token = interaction.Get<std::string>("token", std::string());
        RequireSnowflake(id, "interaction");

        if (token.empty() || token.size() > 512 || token.find_first_of("/ ?#") != std::string::npos)
        {
            throw std::invalid_argument("Interaction table has no valid token");
        }

        return "/interactions/" + id + "/" + token;
    }

    static std::string IsoTimeIn(int64_t seconds)
    {
        const auto target = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
        const std::time_t time = std::chrono::system_clock::to_time_t(target);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buffer;
    }

    static Json::Value PresencePayload(const CTable& options)
    {
        auto input = Json::RenameKeys(TableToJson(options, 0), false);
        Json::Value presence;
        presence.Set("since", Json::Value());
        presence.Set("status", input["status"].String("online"));
        presence.Set("afk", false);
        Json::Array activities;

        if (const auto* activity = input.Find("activity"))
        {
            Json::Value item;
            item.Set("name", (*activity)["name"].String("Legacy Multiplayer"));
            item.Set("type", (*activity)["type"].Integer(0));

            if (const auto* state = activity->Find("state"))
            {
                item.Set("state", state->String());
            }

            activities.push_back(item);
        }

        presence.Set("activities", Json::Value(std::move(activities)));
        return presence;
    }

    void MarkDirty(Session& session, const std::string& scope)
    {
        session.dirtyScopes.insert(scope);
        session.syncAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    }

    void SyncCommands(Session& session)
    {
        const auto application = session.client->ApplicationId();

        if (!session.ready || application.empty() || session.dirtyScopes.empty() || std::chrono::steady_clock::now() < session.syncAt)
        {
            return;
        }

        for (const auto& scope : session.dirtyScopes)
        {
            Json::Array list;

            for (const auto& command : session.commands[scope])
            {
                list.push_back(command.second.definition);
            }

            const Json::Value body(std::move(list));
            const auto path = scope.empty() ? "/applications/" + application + "/commands"
                                            : "/applications/" + application + "/guilds/" + scope + "/commands";
            const auto request = session.client->Enqueue("PUT", path, &body, {});

            if (!request)
            {
                return;
            }

            session.commandSyncs.insert(request);
        }

        session.dirtyScopes.clear();
    }

    void Fire(const char* name, int64_t client, const CValue& data)
    {
        const auto* api = API();

        if (!api || !api->TriggerEvent)
        {
            return;
        }

        try
        {
            CValueStorage storage;
            Value values[2] = {ValueInteger(client), storage.Write(data)};
            api->TriggerEvent(Handle(), name, values, 2);
        }
        catch (const std::exception& error)
        {
            Log.Error("Could not deliver {}: {}", name, error.what());
        }
    }

    void FireError(int64_t client, const std::string& message)
    {
        const auto* api = API();
        Log.Warning("Discord client {}: {}", client, message);

        if (!api || !api->TriggerEvent)
        {
            return;
        }

        CValueStorage storage;
        storage.strings.push_back(message);
        Value values[2] = {ValueInteger(client), ValueStringSized(storage.strings.back().data(), storage.strings.back().size())};
        api->TriggerEvent(Handle(), "onDiscordError", values, 2);
    }

    void HandleEvent(int64_t id, Session& session, const Discord::Event& event)
    {
        if (event.name == "__ERROR")
        {
            return FireError(id, event.data["message"].String());
        }

        if (event.name == "__DISCONNECTED")
        {
            session.ready = false;
            Fire("onDiscordDisconnected", id, ToLua(event.data));
            return;
        }

        if (event.name == "READY" || event.name == "RESUMED")
        {
            const bool first = !session.ready && event.name == "READY";
            session.ready = true;

            if (event.name == "READY")
            {
                for (const auto& scope : session.commands)
                {
                    session.dirtyScopes.insert(scope.first);
                }

                session.syncAt = std::chrono::steady_clock::now();
                const auto user = Json::RenameKeys(event.data["user"], true);
                Log.Info("Discord client {} connected as {}", id, user["username"].String());

                if (first)
                {
                    Fire("onDiscordReady", id, ToLua(user));
                }
            }

            return;
        }

        const auto data = Json::RenameKeys(event.data, true);
        const char* name = nullptr;

        if (event.name == "MESSAGE_CREATE")
        {
            name = "onDiscordMessageCreate";
        }
        else if (event.name == "MESSAGE_UPDATE")
        {
            name = "onDiscordMessageUpdate";
        }
        else if (event.name == "MESSAGE_DELETE")
        {
            name = "onDiscordMessageDelete";
        }
        else if (event.name == "GUILD_MEMBER_ADD")
        {
            name = "onDiscordMemberJoin";
        }
        else if (event.name == "GUILD_MEMBER_REMOVE")
        {
            name = "onDiscordMemberLeave";
        }
        else if (event.name == "MESSAGE_REACTION_ADD")
        {
            name = "onDiscordReactionAdd";
        }
        else if (event.name == "INTERACTION_CREATE")
        {
            name = "onDiscordInteraction";
        }

        if (!name)
        {
            return;
        }

        const auto lua = ToLua(data);
        Fire(name, id, lua);

        if (!m_sessions.count(id) || m_sessions.at(id).get() != &session)
        {
            return;
        }

        if (event.name == "INTERACTION_CREATE" && event.data["type"].Integer() == 2)
        {
            const auto commandName = event.data["data"]["name"].String();
            const auto guild = event.data["guild_id"].String();
            const Command* command = nullptr;

            for (const auto& scope : {guild, std::string()})
            {
                const auto found = session.commands.find(scope);

                if (found != session.commands.end())
                {
                    const auto entry = found->second.find(commandName);

                    if (entry != found->second.end())
                    {
                        command = &entry->second;
                        break;
                    }
                }
            }

            if (command && command->handler && command->handler->IsValid())
            {
                CTable options;
                FlattenOptions(event.data["data"]["options"], options);

                try
                {
                    // Lua may unregister this command or destroy its client during the call.
                    const auto handler = command->handler;
                    handler->Invoke(lua, CValue(options));
                }
                catch (const std::exception& error)
                {
                    Log.Error("Discord command /{} handler failed: {}", commandName, error.what());
                }
            }
        }
    }

    void Pump()
    {
        std::vector<int64_t> ids;

        for (const auto& entry : m_sessions)
        {
            ids.push_back(entry.first);
        }

        for (const auto id : ids)
        {
            auto found = m_sessions.find(id);

            if (found == m_sessions.end())
            {
                continue;
            }

            auto* session = found->second.get();

            for (const auto& event : session->client->TakeEvents())
            {
                HandleEvent(id, *session, event);

                if (!m_sessions.count(id))
                {
                    break;
                }
            }

            found = m_sessions.find(id);

            if (found == m_sessions.end())
            {
                continue;
            }

            session = found->second.get();

            for (auto& result : session->client->TakeResults())
            {
                // Error events and result callbacks may destroy this client.
                const auto current = m_sessions.find(id);
                if (current == m_sessions.end() || current->second.get() != session)
                {
                    break;
                }

                const auto sync = session->commandSyncs.find(result.id);

                if (sync != session->commandSyncs.end())
                {
                    session->commandSyncs.erase(sync);

                    if (result.success)
                    {
                        Log.Info("Discord client {}: {} slash command(s) synced", id, result.data.Items().size());
                    }
                    else
                    {
                        FireError(id, "Slash command sync failed: " + result.error);
                    }

                    continue;
                }

                const auto callback = session->callbacks.find(result.id);

                if (callback == session->callbacks.end())
                {
                    if (!result.success)
                    {
                        FireError(id, result.error);
                    }

                    continue;
                }

                const auto handler = std::move(callback->second);
                session->callbacks.erase(callback);

                if (!handler.IsValid())
                {
                    continue;
                }

                try
                {
                    const auto data = result.success ? ToLua(Json::RenameKeys(result.data, true)) : CValue{};

                    if (result.success)
                    {
                        handler.Invoke(true, data);
                    }
                    else
                    {
                        handler.Invoke(false, CValue{}, result.error);
                    }
                }
                catch (const std::exception& error)
                {
                    Log.Error("Discord callback failed: {}", error.what());
                }

                if (!m_sessions.count(id))
                {
                    break;
                }
            }

            found = m_sessions.find(id);

            if (found != m_sessions.end())
            {
                SyncCommands(*found->second);
            }
        }
    }

    std::filesystem::path m_root;
    std::map<int64_t, std::unique_ptr<Session>> m_sessions;
    int64_t m_nextClient = 0;
};

REGISTER_PLUGIN(CDiscord)
