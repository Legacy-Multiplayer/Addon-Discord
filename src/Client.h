#pragma once

#include "Json.h"
#include "Transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Discord
{
struct Event
{
    std::string name;
    Json::Value data;
};

struct RestResult
{
    uint64_t id = 0;
    bool success = false;
    int status = 0;
    Json::Value data;
    std::string error;
};

class Client
{
public:
    Client(std::string token, int64_t intents, Json::Value presence);

    ~Client();

    Client(const Client&) = delete;

    Client& operator=(const Client&) = delete;

    void Start();

    void Stop();

    bool Connected() const
    {
        return m_connected.load();
    }

    uint64_t Enqueue(const std::string& method, const std::string& path, const Json::Value* body, const std::string& reason);

    void UpdatePresence(const Json::Value& presence);

    std::vector<Event> TakeEvents();

    std::vector<RestResult> TakeResults();

    std::string ApplicationId() const;

    Json::Value User() const;

    static constexpr size_t MaximumQueue = 256;

private:
    struct Job
    {
        uint64_t id;
        std::string method;
        std::string path;
        std::string body;
        bool hasBody;
        std::string reason;
    };

    void GatewayLoop();

    void HeartbeatLoop();

    void RestLoop();

    void Push(Event event);

    void PushError(const std::string& message);

    bool Wait(std::chrono::milliseconds duration);

    bool SendHeartbeat();

    bool SendJson(const Json::Value& payload);

    Json::Value IdentifyPayload() const;

    RestResult Execute(const Job& job);

    std::string m_token;
    int64_t m_intents;

    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_connected{false};
    bool m_started = false;

    std::thread m_gatewayThread;
    std::thread m_heartbeatThread;
    std::thread m_restThread;

    Transport::WebSocket m_socket;
    Transport::Https m_https;

    mutable std::mutex m_stateMutex;
    std::condition_variable m_stateSignal;
    Json::Value m_presence;
    std::string m_sessionId;
    std::string m_resumeHost;
    std::string m_applicationId;
    Json::Value m_user;
    int64_t m_sequence = -1;
    int64_t m_heartbeatInterval = 0;
    std::chrono::steady_clock::time_point m_nextBeat;
    bool m_acknowledged = true;

    std::mutex m_eventMutex;
    std::vector<Event> m_events;

    std::mutex m_jobMutex;
    std::condition_variable m_jobSignal;
    std::deque<Job> m_jobs;
    std::vector<RestResult> m_results;
    uint64_t m_nextJob = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> m_buckets;
};

int64_t IntentValue(const std::string& name);

} // namespace Discord
