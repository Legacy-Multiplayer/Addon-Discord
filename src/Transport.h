#pragma once

#include <map>
#include <memory>
#include <string>

namespace Transport
{
struct Response
{
    bool ok = false;
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;
};

class Https
{
public:
    Https();

    ~Https();

    Https(const Https&) = delete;

    Https& operator=(const Https&) = delete;

    Response Request(const std::string& host, const std::string& method, const std::string& path,
        const std::map<std::string, std::string>& headers, const std::string& body);

    void Abort();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

enum class Frame
{
    Text,
    Closed,
    Failed
};

class WebSocket
{
public:
    WebSocket();

    ~WebSocket();

    WebSocket(const WebSocket&) = delete;

    WebSocket& operator=(const WebSocket&) = delete;

    bool Connect(const std::string& host, const std::string& path, std::string& error);

    bool Send(const std::string& text);

    Frame Receive(std::string& text, int& closeCode, std::string& closeReason);

    void Close(int code);

    void Abort();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace Transport
