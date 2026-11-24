#include "Transport.h"
#include <cstdio>

int main()
{
    Transport::Https https;
    https.Abort();
    https.Abort();
    const auto response = https.Request("discord.com", "GET", "/api/v10/gateway", {}, "");
    if (response.ok || response.error.empty()) return 1;

    Transport::WebSocket socket;
    if (socket.Send("{}")) return 1;
    std::string text, reason;
    int code = 0;
    if (socket.Receive(text, code, reason) == Transport::Frame::Text) return 1;
    socket.Close(1000);
    socket.Abort();
    socket.Abort();
    std::puts("Idle close, repeated cancellation and cancelled HTTPS checks passed");
    return 0;
}
