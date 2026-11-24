#include "Json.h"
#include "Transport.h"

#include <cstdio>
#include <chrono>
#include <thread>

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;

    Transport::Https https;
    const auto response = https.Request("discord.com", "GET", "/api/v10/gateway", {{"User-Agent", "DiscordBot (probe, 1.0)"}}, "");

    if (!response.ok || response.status != 200)
    {
        std::printf("HTTPS FAILED: status %d, %s\n", response.status, response.error.c_str());
        ++failures;
    }
    else
    {
        const auto url = Json::Parse(response.body)["url"].String();
        std::printf("HTTPS ok: GET /api/v10/gateway -> %s\n", url.c_str());
    }

    Transport::WebSocket socket;
    std::string error;

    if (!socket.Connect("gateway.discord.gg", "/?v=10&encoding=json", error))
    {
        std::printf("WEBSOCKET FAILED: %s\n", error.c_str());
        return 1;
    }

    std::string text;
    int code = 0;
    std::string reason;
    const auto frame = socket.Receive(text, code, reason);

    if (frame != Transport::Frame::Text)
    {
        std::printf("WEBSOCKET FAILED: no Hello frame (%s)\n", reason.c_str());
        return 1;
    }

    const auto hello = Json::Parse(text);
    std::printf("WebSocket ok: op %lld, heartbeat_interval %lld ms\n", static_cast<long long>(hello["op"].Integer(-1)),
        static_cast<long long>(hello["d"]["heartbeat_interval"].Integer(-1)));

    if (hello["op"].Integer(-1) != 10)
    {
        ++failures;
    }

    // Heartbeats are valid before Identify: exercise actual client-to-server
    // WebSocket writes without a bot token or application messages.
    if (!socket.Send(R"({"op":1,"d":null})"))
    {
        std::puts("WEBSOCKET FAILED: heartbeat send");
        return 1;
    }
    if (socket.Receive(text, code, reason) != Transport::Frame::Text
        || Json::Parse(text)["op"].Integer(-1) != 11)
    {
        std::puts("WEBSOCKET FAILED: no heartbeat ACK");
        return 1;
    }
    std::puts("WebSocket heartbeat ACK ok");

    // Abort must wake a blocked receive, and the same public object can reconnect.
    std::thread reader([&]() { socket.Receive(text, code, reason); });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    std::puts("Cancelling blocked WebSocket receive...");
    socket.Abort();
    std::puts("WebSocket abort returned");
    reader.join();
    if (!socket.Connect("gateway.discord.gg", "/?v=10&encoding=json", error)
        || socket.Receive(text, code, reason) != Transport::Frame::Text
        || Json::Parse(text)["op"].Integer(-1) != 10)
    {
        std::puts("WEBSOCKET FAILED: reconnect after cancellation");
        return 1;
    }
    std::puts("WebSocket blocked-read cancellation and reconnect ok");
    socket.Close(1000);
    socket.Abort();
    return failures ? 1 : 0;
}
