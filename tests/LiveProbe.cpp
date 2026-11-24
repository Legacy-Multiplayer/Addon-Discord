#include "Json.h"
#include "Transport.h"

#include <cstdio>

int main()
{
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

    socket.Close(1000);
    socket.Abort();
    return failures ? 1 : 0;
}
