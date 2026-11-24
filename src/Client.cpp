#include "Client.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>

namespace Discord
{
namespace
{
constexpr const char* GatewayHost = "gateway.discord.gg";
constexpr const char* GatewayPath = "/?v=10&encoding=json";
constexpr const char* ApiHost = "discord.com";
constexpr const char* ApiPrefix = "/api/v10";

const std::set<std::string>& ForwardedEvents()
{
    static const std::set<std::string> names = {
        "READY",
        "RESUMED",
        "MESSAGE_CREATE",
        "MESSAGE_UPDATE",
        "MESSAGE_DELETE",
        "INTERACTION_CREATE",
        "GUILD_MEMBER_ADD",
        "GUILD_MEMBER_REMOVE",
        "MESSAGE_REACTION_ADD",
    };

    return names;
}

bool FatalClose(int code)
{
    return code == 4004 || code == 4010 || code == 4011 || code == 4012 || code == 4013 || code == 4014;
}

std::string CloseMeaning(int code)
{
    switch (code)
    {
    case 4004:
        return "Authentication failed: the bot token is invalid";
    case 4010:
        return "Invalid shard";
    case 4011:
        return "Sharding required";
    case 4012:
        return "Invalid gateway API version";
    case 4013:
        return "Invalid intents";
    case 4014:
        return "Disallowed intents: enable the privileged intents (Message Content, Server Members) in the Discord Developer Portal";
    default:
        return {};
    }
}

std::string StripScheme(const std::string& url)
{
    auto host = url;
    const auto scheme = host.find("://");

    if (scheme != std::string::npos)
    {
        host = host.substr(scheme + 3);
    }

    const auto slash = host.find('/');

    if (slash != std::string::npos)
    {
        host = host.substr(0, slash);
    }

    return host;
}

std::string PercentEncode(const std::string& text)
{
    std::string output;

    for (const char raw : text)
    {
        const auto character = static_cast<unsigned char>(raw);

        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~')
        {
            output.push_back(raw);
        }
        else
        {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", character);
            output += buffer;
        }
    }

    return output;
}

double ParseSeconds(const std::string& text, double fallback)
{
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    return end && end != text.c_str() && *end == '\0' && std::isfinite(value) && value >= 0
        ? std::min(value, 86400.0) : fallback;
}

} // namespace

int64_t IntentValue(const std::string& name)
{
    static const std::map<std::string, int64_t> intents = {
        {"GUILDS", 1 << 0},
        {"GUILD_MEMBERS", 1 << 1},
        {"GUILD_MODERATION", 1 << 2},
        {"GUILD_EXPRESSIONS", 1 << 3},
        {"GUILD_INTEGRATIONS", 1 << 4},
        {"GUILD_WEBHOOKS", 1 << 5},
        {"GUILD_INVITES", 1 << 6},
        {"GUILD_VOICE_STATES", 1 << 7},
        {"GUILD_PRESENCES", 1 << 8},
        {"GUILD_MESSAGES", 1 << 9},
        {"GUILD_MESSAGE_REACTIONS", 1 << 10},
        {"GUILD_MESSAGE_TYPING", 1 << 11},
        {"DIRECT_MESSAGES", 1 << 12},
        {"DIRECT_MESSAGE_REACTIONS", 1 << 13},
        {"DIRECT_MESSAGE_TYPING", 1 << 14},
        {"MESSAGE_CONTENT", 1 << 15},
        {"GUILD_SCHEDULED_EVENTS", 1 << 16},
        {"AUTO_MODERATION_CONFIGURATION", 1 << 20},
        {"AUTO_MODERATION_EXECUTION", 1 << 21},
    };

    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::toupper(character));
    });

    const auto found = intents.find(key);
    return found == intents.end() ? -1 : found->second;
}

Client::Client(std::string token, int64_t intents, Json::Value presence)
    : m_token(std::move(token)), m_intents(intents), m_presence(std::move(presence))
{
}

Client::~Client()
{
    Stop();
}

void Client::Start()
{
    if (m_started)
    {
        return;
    }

    m_started = true;
    m_gatewayThread = std::thread([this]()
    {
        GatewayLoop();
    });
    m_heartbeatThread = std::thread([this]()
    {
        HeartbeatLoop();
    });
    m_restThread = std::thread([this]()
    {
        RestLoop();
    });
}

void Client::Stop()
{
    if (m_stopping.exchange(true))
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_heartbeatInterval = 0;
    }

    m_stateSignal.notify_all();
    m_jobSignal.notify_all();
    m_socket.Close(1000);
    m_socket.Abort();
    m_https.Abort();

    for (auto* thread : {&m_gatewayThread, &m_heartbeatThread, &m_restThread})
    {
        if (thread->joinable())
        {
            thread->join();
        }
    }

    m_connected = false;
}

bool Client::Wait(std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lock(m_stateMutex);
    m_stateSignal.wait_for(lock, duration, [this]()
    {
        return m_stopping.load();
    });
    return !m_stopping.load();
}

void Client::Push(Event event)
{
    std::lock_guard<std::mutex> lock(m_eventMutex);

    if (m_events.size() < 4096)
    {
        m_events.push_back(std::move(event));
    }
}

void Client::PushError(const std::string& message)
{
    Json::Value data;
    data.Set("message", message);
    Push({"__ERROR", std::move(data)});
}

std::vector<Event> Client::TakeEvents()
{
    std::lock_guard<std::mutex> lock(m_eventMutex);
    std::vector<Event> events;
    events.swap(m_events);
    return events;
}

std::vector<RestResult> Client::TakeResults()
{
    std::lock_guard<std::mutex> lock(m_jobMutex);
    std::vector<RestResult> results;
    results.swap(m_results);
    return results;
}

std::string Client::ApplicationId() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_applicationId;
}

Json::Value Client::User() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_user;
}

bool Client::SendJson(const Json::Value& payload)
{
    return m_socket.Send(Json::Dump(payload));
}

bool Client::SendHeartbeat()
{
    int64_t sequence = -1;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        sequence = m_sequence;
    }

    Json::Value payload;
    payload.Set("op", 1);
    payload.Set("d", sequence >= 0 ? Json::Value(sequence) : Json::Value());
    return SendJson(payload);
}

Json::Value Client::IdentifyPayload() const
{
    Json::Value properties;
    properties.Set("os", "windows");
    properties.Set("browser", "legacy-addon-discord");
    properties.Set("device", "legacy-addon-discord");

    Json::Value data;
    data.Set("token", m_token);
    data.Set("intents", m_intents);
    data.Set("properties", properties);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);

        if (m_presence.IsObject())
        {
            data.Set("presence", m_presence);
        }
    }

    Json::Value payload;
    payload.Set("op", 2);
    payload.Set("d", data);
    return payload;
}

void Client::UpdatePresence(const Json::Value& presence)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_presence = presence;
    }

    if (m_connected.load())
    {
        Json::Value payload;
        payload.Set("op", 3);
        payload.Set("d", presence);
        SendJson(payload);
    }
}

void Client::GatewayLoop()
{
    int backoffSeconds = 1;
    std::mt19937 random(std::random_device{}());

    while (!m_stopping.load())
    {
        std::string host;
        bool resuming = false;

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            resuming = !m_sessionId.empty() && m_sequence >= 0;
            host = resuming && !m_resumeHost.empty() ? m_resumeHost : GatewayHost;
        }

        std::string error;

        if (!m_socket.Connect(host, GatewayPath, error))
        {
            if (m_stopping.load())
            {
                break;
            }

            PushError("Gateway connection failed: " + error + ". Retrying in " + std::to_string(backoffSeconds) + "s.");

            if (!Wait(std::chrono::seconds(backoffSeconds)))
            {
                break;
            }

            backoffSeconds = std::min(backoffSeconds * 2, 60);
            continue;
        }

        bool reconnect = true;
        int closeCode = 0;
        std::string closeReason;

        while (!m_stopping.load())
        {
            std::string text;
            const auto frame = m_socket.Receive(text, closeCode, closeReason);

            if (frame != Transport::Frame::Text)
            {
                break;
            }

            Json::Value payload;

            try
            {
                payload = Json::Parse(text);
            }
            catch (const std::exception& problem)
            {
                PushError(std::string("Invalid gateway payload: ") + problem.what());
                continue;
            }

            const int64_t op = payload["op"].Integer(-1);

            if (op == 10)
            {
                const int64_t interval = payload["d"]["heartbeat_interval"].Integer(41250);
                std::uniform_real_distribution<double> jitter(0.0, 1.0);

                {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_heartbeatInterval = interval;
                    m_acknowledged = true;
                    m_nextBeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(interval * jitter(random)));
                }

                m_stateSignal.notify_all();

                if (resuming)
                {
                    Json::Value data;
                    {
                        std::lock_guard<std::mutex> lock(m_stateMutex);
                        data.Set("token", m_token);
                        data.Set("session_id", m_sessionId);
                        data.Set("seq", m_sequence);
                    }
                    Json::Value resume;
                    resume.Set("op", 6);
                    resume.Set("d", data);
                    SendJson(resume);
                }
                else
                {
                    SendJson(IdentifyPayload());
                }
            }
            else if (op == 11)
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_acknowledged = true;
            }
            else if (op == 1)
            {
                SendHeartbeat();
            }
            else if (op == 7)
            {
                m_socket.Close(4900);
                break;
            }
            else if (op == 9)
            {
                const bool resumable = payload["d"].Bool(false);

                if (!resumable)
                {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_sessionId.clear();
                    m_sequence = -1;
                }

                std::uniform_int_distribution<int> delay(1000, 5000);
                Wait(std::chrono::milliseconds(delay(random)));
                m_socket.Close(4901);
                break;
            }
            else if (op == 0)
            {
                const auto name = payload["t"].String();

                {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    const int64_t sequence = payload["s"].Integer(-1);

                    if (sequence >= 0)
                    {
                        m_sequence = sequence;
                    }
                }

                if (name == "READY")
                {
                    const auto& data = payload["d"];

                    {
                        std::lock_guard<std::mutex> lock(m_stateMutex);
                        m_sessionId = data["session_id"].String();
                        m_resumeHost = StripScheme(data["resume_gateway_url"].String());
                        m_applicationId = data["application"]["id"].String();
                        m_user = data["user"];
                    }

                    m_connected = true;
                    backoffSeconds = 1;
                }
                else if (name == "RESUMED")
                {
                    m_connected = true;
                    backoffSeconds = 1;
                }

                if (ForwardedEvents().count(name))
                {
                    Push({name, payload["d"]});
                }
            }
        }

        const bool wasConnected = m_connected.exchange(false);

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_heartbeatInterval = 0;

            if (closeCode == 4007 || closeCode == 4009)
            {
                m_sessionId.clear();
                m_sequence = -1;
            }
        }

        m_socket.Abort();

        if (m_stopping.load())
        {
            break;
        }

        if (FatalClose(closeCode))
        {
            reconnect = false;
            PushError(CloseMeaning(closeCode) + " (close code " + std::to_string(closeCode) + ").");
        }

        Json::Value data;
        data.Set("code", closeCode);
        data.Set("reason", closeReason);
        data.Set("reconnecting", reconnect);
        data.Set("wasConnected", wasConnected);
        Push({"__DISCONNECTED", data});

        if (!reconnect)
        {
            break;
        }

        if (!Wait(std::chrono::seconds(wasConnected ? 1 : backoffSeconds)))
        {
            break;
        }

        if (!wasConnected)
        {
            backoffSeconds = std::min(backoffSeconds * 2, 60);
        }
    }
}

void Client::HeartbeatLoop()
{
    std::unique_lock<std::mutex> lock(m_stateMutex);

    while (!m_stopping.load())
    {
        if (m_heartbeatInterval <= 0)
        {
            m_stateSignal.wait(lock);
            continue;
        }

        const auto due = m_nextBeat;

        if (std::chrono::steady_clock::now() < due)
        {
            m_stateSignal.wait_until(lock, due);
            continue;
        }

        if (!m_acknowledged)
        {
            m_heartbeatInterval = 0;
            lock.unlock();
            m_socket.Abort();
            lock.lock();
            continue;
        }

        m_acknowledged = false;
        m_nextBeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_heartbeatInterval);
        lock.unlock();
        SendHeartbeat();
        lock.lock();
    }
}

uint64_t Client::Enqueue(const std::string& method, const std::string& path, const Json::Value* body, const std::string& reason)
{
    std::lock_guard<std::mutex> lock(m_jobMutex);

    if (m_stopping.load() || m_jobs.size() >= MaximumQueue)
    {
        return 0;
    }

    const uint64_t id = ++m_nextJob;
    m_jobs.push_back({id, method, path, body ? Json::Dump(*body) : std::string(), body != nullptr, reason});
    m_jobSignal.notify_one();
    return id;
}

void Client::RestLoop()
{
    while (true)
    {
        Job job;

        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobSignal.wait(lock, [this]()
            {
                return m_stopping.load() || !m_jobs.empty();
            });

            if (m_stopping.load())
            {
                return;
            }

            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }

        auto result = Execute(job);

        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_results.push_back(std::move(result));
    }
}

RestResult Client::Execute(const Job& job)
{
    RestResult result;
    result.id = job.id;

    auto route = job.method + " " + job.path.substr(0, job.path.find('?'));

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bot " + m_token},
        {"User-Agent", "DiscordBot (https://github.com/Legacy-Multiplayer, 1.0)"},
    };

    if (job.hasBody)
    {
        headers["Content-Type"] = "application/json";
    }

    if (!job.reason.empty())
    {
        headers["X-Audit-Log-Reason"] = PercentEncode(job.reason.substr(0, 512));
    }

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        std::chrono::steady_clock::time_point blocked;

        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            const auto found = m_buckets.find(route);

            if (found != m_buckets.end())
            {
                blocked = found->second;
            }
        }

        const auto now = std::chrono::steady_clock::now();

        if (blocked > now && !Wait(std::chrono::duration_cast<std::chrono::milliseconds>(blocked - now)))
        {
            result.error = "Client stopped";
            return result;
        }

        auto response = m_https.Request(ApiHost, job.method, ApiPrefix + job.path, headers, job.body);

        if (m_stopping.load())
        {
            result.error = "Client stopped";
            return result;
        }

        if (!response.ok)
        {
            if (attempt < 2 && Wait(std::chrono::seconds(1)))
            {
                continue;
            }

            result.error = response.error;
            return result;
        }

        result.status = response.status;
        const auto remaining = response.headers.find("x-ratelimit-remaining");
        const auto resetAfter = response.headers.find("x-ratelimit-reset-after");

        if (remaining != response.headers.end() && remaining->second == "0" && resetAfter != response.headers.end())
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            m_buckets[route] = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(ParseSeconds(resetAfter->second, 1.0) * 1000));
        }

        Json::Value data;

        if (!response.body.empty())
        {
            try
            {
                data = Json::Parse(response.body);
            }
            catch (const std::exception&)
            {
                data = Json::Value(response.body);
            }
        }

        if (response.status == 429)
        {
            double wait = data["retry_after"].Number(-1.0);

            if (wait < 0)
            {
                const auto header = response.headers.find("retry-after");
                wait = header != response.headers.end() ? ParseSeconds(header->second, 1.0) : 1.0;
            }

            if (!Wait(std::chrono::milliseconds(static_cast<int64_t>(std::min(wait, 60.0) * 1000) + 50)))
            {
                result.error = "Client stopped";
                return result;
            }

            continue;
        }

        if (response.status >= 500 && attempt < 2)
        {
            if (!Wait(std::chrono::seconds(1 + attempt)))
            {
                result.error = "Client stopped";
                return result;
            }

            continue;
        }

        if (response.status >= 200 && response.status < 300)
        {
            result.success = true;
            result.data = std::move(data);
            return result;
        }

        result.error = "HTTP " + std::to_string(response.status);

        if (data.IsObject())
        {
            const auto message = data["message"].String();

            if (!message.empty())
            {
                result.error += ": " + message;
            }

            if (const auto* code = data.Find("code"))
            {
                result.error += " (code " + code->String() + ")";
            }

            if (const auto* errors = data.Find("errors"))
            {
                result.error += " " + Json::Dump(*errors).substr(0, 400);
            }
        }

        result.data = std::move(data);
        return result;
    }

    result.error = "Rate limited too many times";
    return result;
}

} // namespace Discord
