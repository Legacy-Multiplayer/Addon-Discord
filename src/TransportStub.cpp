#include "Transport.h"

namespace Transport
{
struct Https::State
{
};

Https::Https() : m_state(std::make_unique<State>())
{
}

Https::~Https() = default;

Response Https::Request(const std::string&, const std::string&, const std::string&, const std::map<std::string, std::string>&, const std::string&)
{
    Response response;
    response.error = "HTTPS transport is not implemented on this platform yet";
    return response;
}

void Https::Abort()
{
}

struct WebSocket::State
{
};

WebSocket::WebSocket() : m_state(std::make_unique<State>())
{
}

WebSocket::~WebSocket() = default;

bool WebSocket::Connect(const std::string&, const std::string&, std::string& error)
{
    error = "WebSocket transport is not implemented on this platform yet";
    return false;
}

bool WebSocket::Send(const std::string&)
{
    return false;
}

Frame WebSocket::Receive(std::string&, int&, std::string&)
{
    return Frame::Failed;
}

void WebSocket::Close(int)
{
}

void WebSocket::Abort()
{
}

} // namespace Transport
