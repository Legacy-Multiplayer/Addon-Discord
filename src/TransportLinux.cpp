#include "Transport.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <mutex>
#include <thread>

namespace Transport
{
namespace
{
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using Error = boost::system::error_code;
constexpr size_t MaxMessage = 32 * 1024 * 1024;
constexpr size_t MaxQueuedBytes = 32 * 1024 * 1024;
constexpr size_t MaxOutbound = 16 * 1024;

void ConfigureTLS(ssl::context& context)
{
    context.set_default_verify_paths();
    context.set_verify_mode(ssl::verify_peer);
    if (!SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION))
    {
        throw std::runtime_error("Could not configure TLS 1.2 minimum");
    }
}

ssl::context MakeTLSContext()
{
    ssl::context context(ssl::context::tls_client);
    ConfigureTLS(context);
    return context;
}

struct Endpoint
{
    std::string name;
    std::string port = "443";
    std::string authority;
};

Endpoint ParseEndpoint(const std::string& host)
{
    Endpoint endpoint;
    endpoint.authority = host;
    const auto colon = host.find(':');
    endpoint.name = host.substr(0, colon);
    if (colon != std::string::npos)
    {
        endpoint.port = host.substr(colon + 1);
        if (endpoint.port.empty() || endpoint.port.size() > 5
            || !std::all_of(endpoint.port.begin(), endpoint.port.end(), [](unsigned char c) { return std::isdigit(c); })
            || std::stoi(endpoint.port) < 1 || std::stoi(endpoint.port) > 65535)
        {
            throw std::invalid_argument("Invalid TLS peer port");
        }
    }
    return endpoint;
}

void ConfigurePeer(SSL* handle, const std::string& host)
{
    if (host.empty() || host.find_first_of("/\\\r\n") != std::string::npos
        || host.find('\0') != std::string::npos || !SSL_set_tlsext_host_name(handle, host.c_str()))
    {
        throw std::runtime_error("Invalid TLS peer hostname");
    }
}

class HttpOperation : public std::enable_shared_from_this<HttpOperation>
{
public:
    HttpOperation() : context(MakeTLSContext()), resolver(io), stream(io, context), timer(io)
    {
    }

    Response Run(const std::string& host, const std::string& method, const std::string& path,
        const std::map<std::string, std::string>& headers, const std::string& body)
    {
        const auto endpoint = ParseEndpoint(host);
        ConfigurePeer(stream.native_handle(), endpoint.name);
        stream.set_verify_callback(ssl::host_name_verification(endpoint.name));
        request.version(11);
        request.method_string(method);
        request.target(path);
        request.set(http::field::host, host);
        request.set(http::field::user_agent, "Legacy-Addon-Discord/1.0");
        for (const auto& header : headers) request.set(header.first, header.second);
        request.keep_alive(false);
        request.body() = body;
        request.prepare_payload();
        parser.body_limit(MaxMessage);

        net::post(io, [this, endpoint]()
        {
            if (finished) return;
            timer.expires_after(std::chrono::seconds(20));
            timer.async_wait([this](Error ec)
            {
                if (!ec) Finish("HTTPS request timed out");
            });
            resolver.async_resolve(endpoint.name, endpoint.port, [this](Error ec, tcp::resolver::results_type endpoints)
            {
                if (Fail(ec, "HTTPS resolve")) return;
                beast::get_lowest_layer(stream).async_connect(endpoints, [this](Error ec, const tcp::endpoint&)
                {
                    if (Fail(ec, "HTTPS connect")) return;
                    stream.async_handshake(ssl::stream_base::client, [this](Error ec)
                    {
                        if (Fail(ec, "HTTPS TLS handshake")) return;
                        http::async_write(stream, request, [this](Error ec, size_t)
                        {
                            if (Fail(ec, "HTTPS write")) return;
                            http::async_read(stream, buffer, parser, [this](Error ec, size_t)
                            {
                                if (Fail(ec, "HTTPS read")) return;
                                auto message = parser.release();
                                result.status = message.result_int();
                                result.body = std::move(message.body());
                                for (const auto& header : message.base())
                                {
                                    std::string key(header.name_string().data(), header.name_string().size());
                                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
                                    {
                                        return static_cast<char>(std::tolower(c));
                                    });
                                    result.headers[key] = std::string(header.value().data(), header.value().size());
                                }
                                result.ok = true;
                                Finish("");
                            });
                        });
                    });
                });
            });
        });
        io.run();
        return std::move(result);
    }

    void Cancel()
    {
        const std::weak_ptr<HttpOperation> weak = shared_from_this();
        net::post(io, [weak]()
        {
            if (const auto operation = weak.lock()) operation->Finish("HTTPS request cancelled");
        });
    }

private:
    bool Fail(Error ec, const char* stage)
    {
        if (finished) return true;
        if (!ec) return false;
        Finish(std::string(stage) + ": " + ec.message());
        return true;
    }

    void Finish(std::string error)
    {
        if (finished) return;
        finished = true;
        result.error = std::move(error);
        Error ignored;
        timer.cancel(ignored);
        resolver.cancel();
        beast::get_lowest_layer(stream).socket().cancel(ignored);
        beast::get_lowest_layer(stream).socket().close(ignored);
    }

    net::io_context io;
    ssl::context context;
    tcp::resolver resolver;
    beast::ssl_stream<beast::tcp_stream> stream;
    net::steady_timer timer;
    http::request<http::string_body> request;
    http::response_parser<http::string_body> parser;
    beast::flat_buffer buffer;
    Response result;
    bool finished = false; // Only the I/O thread touches native operation state.
};

// One event-loop thread owns every WebSocket and TLS operation. Public callers
// exchange bounded queues with it; concurrent native reads/writes never race.
class Gateway : public std::enable_shared_from_this<Gateway>
{
public:
    Gateway() : context(MakeTLSContext()), resolver(io), socket(io, context), timer(io)
    {
        socket.read_message_max(MaxMessage);
    }

    ~Gateway()
    {
        Stop();
    }

    bool Connect(const std::string& host, const std::string& path, std::string& error)
    {
        const auto endpoint = ParseEndpoint(host);
        ConfigurePeer(socket.next_layer().native_handle(), endpoint.name);
        socket.next_layer().set_verify_callback(ssl::host_name_verification(endpoint.name));
        net::post(io, [this, endpoint, path]() { Start(endpoint, path); });
        {
            std::lock_guard<std::mutex> lock(joinMutex);
            worker = std::thread([this]()
            {
                try { io.run(); }
                catch (const std::exception&) { Finish(Frame::Failed, 0, "WebSocket I/O failed"); }
            });
        }
        std::unique_lock<std::mutex> lock(mutex);
        signal.wait(lock, [this]() { return ready || done; });
        error = reason;
        return ready && !done;
    }

    bool Send(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!ready || done || closing || outgoing.size() >= 128 || text.size() > MaxOutbound) return false;
        outgoing.push_back(text);
        net::post(io, [this]() { Write(); });
        return true;
    }

    Frame Receive(std::string& text, int& code, std::string& error)
    {
        std::unique_lock<std::mutex> lock(mutex);
        signal.wait(lock, [this]() { return !incoming.empty() || done; });
        if (!incoming.empty())
        {
            text = std::move(incoming.front());
            queuedBytes -= text.size();
            incoming.pop_front();
            return Frame::Text;
        }
        code = closeCode;
        error = reason;
        return finalFrame;
    }

    void Close(int code)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (done || !ready || closing) return;
            closing = true;
            requestedClose = code;
        }
        net::post(io, [this]() { Write(); });
    }

    void Stop()
    {
        const std::weak_ptr<Gateway> weak = weak_from_this();
        net::post(io, [weak]()
        {
            if (const auto gateway = weak.lock()) gateway->Finish(Frame::Closed, 1000, "WebSocket cancelled");
        });
        std::lock_guard<std::mutex> lock(joinMutex);
        if (worker.joinable()) worker.join();
    }

private:
    void Start(const Endpoint& host, const std::string& path)
    {
        if (finished) return;
        timer.expires_after(std::chrono::seconds(20));
        timer.async_wait([this](Error ec)
        {
            if (!ec) Finish(Frame::Failed, 0, "Gateway connection timed out");
        });
        resolver.async_resolve(host.name, host.port, [this, host, path](Error ec, tcp::resolver::results_type endpoints)
        {
            if (Fail(ec, "Gateway resolve")) return;
            beast::get_lowest_layer(socket).async_connect(endpoints, [this, host, path](Error ec, const tcp::endpoint&)
            {
                if (Fail(ec, "Gateway connect")) return;
                socket.next_layer().async_handshake(ssl::stream_base::client, [this, host, path](Error ec)
                {
                    if (Fail(ec, "Gateway TLS handshake")) return;
                    beast::get_lowest_layer(socket).expires_never();
                    socket.set_option(websocket::stream_base::timeout{
                        std::chrono::seconds(20), std::chrono::seconds(90), true});
                    socket.set_option(websocket::stream_base::decorator([](websocket::request_type& request)
                    {
                        request.set(http::field::user_agent, "Legacy-Addon-Discord/1.0");
                    }));
                    socket.async_handshake(host.authority, path, [this](Error ec)
                    {
                        if (Fail(ec, "Gateway WebSocket handshake")) return;
                        Error ignored;
                        timer.cancel(ignored);
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            ready = true;
                        }
                        signal.notify_all();
                        Read();
                    });
                });
            });
        });
    }

    bool Fail(Error ec, const char* stage)
    {
        if (finished) return true;
        if (!ec) return false;
        if (ec == websocket::error::closed)
        {
            const auto closed = socket.reason();
            Finish(Frame::Closed, static_cast<int>(closed.code), std::string(closed.reason.data(), closed.reason.size()));
        }
        else Finish(Frame::Failed, 0, std::string(stage) + ": " + ec.message());
        return true;
    }

    void Read()
    {
        socket.async_read(buffer, [this](Error ec, size_t)
        {
            if (Fail(ec, "Gateway read")) return;
            if (!socket.got_text())
            {
                Finish(Frame::Failed, 0, "Unexpected binary Gateway message");
                return;
            }
            auto text = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            bool overflow;
            {
                std::lock_guard<std::mutex> lock(mutex);
                overflow = incoming.size() >= 4096 || text.size() > MaxQueuedBytes - queuedBytes;
                if (!overflow)
                {
                    queuedBytes += text.size();
                    incoming.push_back(std::move(text));
                }
            }
            if (overflow)
            {
                Finish(Frame::Failed, 0, "Gateway receive queue limit exceeded");
                return;
            }
            signal.notify_all();
            Read();
        });
    }

    void Write()
    {
        if (finished || writing || closeStarted) return;
        bool close = false;
        int code = 1000;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (done || !ready) return;
            if (outgoing.empty())
            {
                if (!closing) return;
                close = true;
                code = requestedClose;
            }
            else
            {
                writing = true;
                activeWrite = std::move(outgoing.front());
                outgoing.pop_front();
            }
        }
        if (close)
        {
            closeStarted = true;
            websocket::close_reason reason;
            reason.code = static_cast<websocket::close_code>(code);
            socket.async_close(reason, [this, code](Error ec)
            {
                if (Fail(ec, "Gateway close")) return;
                Finish(Frame::Closed, code, "WebSocket closed");
            });
            return;
        }
        socket.text(true);
        socket.async_write(net::buffer(activeWrite), [this](Error ec, size_t)
        {
            writing = false;
            activeWrite.clear();
            if (!Fail(ec, "Gateway write")) Write();
        });
    }

    void Finish(Frame frame, int code, std::string error)
    {
        if (finished) return;
        finished = true;
        Error ignored;
        timer.cancel(ignored);
        resolver.cancel();
        beast::get_lowest_layer(socket).socket().cancel(ignored);
        beast::get_lowest_layer(socket).socket().close(ignored);
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
            closeCode = code;
            reason = std::move(error);
            finalFrame = frame;
        }
        signal.notify_all();
    }

    net::io_context io;
    ssl::context context;
    tcp::resolver resolver;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> socket;
    net::steady_timer timer;
    beast::flat_buffer buffer;
    std::thread worker;
    std::mutex joinMutex;
    std::mutex mutex;
    std::condition_variable signal;
    std::deque<std::string> incoming;
    std::deque<std::string> outgoing;
    std::string activeWrite;
    size_t queuedBytes = 0;
    std::string reason;
    Frame finalFrame = Frame::Failed;
    int closeCode = 0;
    int requestedClose = 1000;
    bool ready = false;
    bool done = false;
    bool closing = false;
    bool finished = false; // I/O thread only, as are writing/closeStarted.
    bool writing = false;
    bool closeStarted = false;
};
} // namespace

struct Https::State
{
    std::mutex mutex;
    std::shared_ptr<HttpOperation> active;
    bool aborted = false;
};

Https::Https() : m_state(std::make_unique<State>()) {}
Https::~Https() { Abort(); }

Response Https::Request(const std::string& host, const std::string& method, const std::string& path,
    const std::map<std::string, std::string>& headers, const std::string& body)
{
    Response result;
    std::shared_ptr<HttpOperation> operation;
    try
    {
        operation = std::make_shared<HttpOperation>();
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->aborted) { result.error = "HTTPS request cancelled"; return result; }
            if (m_state->active) { result.error = "HTTPS request already in progress"; return result; }
            m_state->active = operation;
        }
        result = operation->Run(host, method, path, headers, body);
    }
    catch (const std::exception& error) { result.error = std::string("HTTPS transport: ") + error.what(); }
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->active == operation) m_state->active.reset();
    }
    return result;
}

void Https::Abort()
{
    std::shared_ptr<HttpOperation> operation;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->aborted = true;
        operation = m_state->active;
    }
    if (operation) operation->Cancel();
}

struct WebSocket::State
{
    std::mutex mutex;
    std::shared_ptr<Gateway> gateway;
    uint64_t generation = 0;
};

WebSocket::WebSocket() : m_state(std::make_unique<State>()) {}
WebSocket::~WebSocket() { Abort(); }

bool WebSocket::Connect(const std::string& host, const std::string& path, std::string& error)
{
    Abort();
    std::shared_ptr<Gateway> gateway;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        generation = m_state->generation;
    }
    try
    {
        gateway = std::make_shared<Gateway>();
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (generation != m_state->generation) { error = "Gateway connection cancelled"; return false; }
            m_state->gateway = gateway;
        }
        return gateway->Connect(host, path, error);
    }
    catch (const std::exception& problem)
    {
        error = std::string("Gateway transport: ") + problem.what();
        if (gateway) gateway->Stop();
        return false;
    }
}

bool WebSocket::Send(const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->gateway && m_state->gateway->Send(text);
}

Frame WebSocket::Receive(std::string& text, int& code, std::string& error)
{
    std::shared_ptr<Gateway> gateway;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        gateway = m_state->gateway;
    }
    if (!gateway) { code = 0; error = "Gateway is not connected"; return Frame::Failed; }
    return gateway->Receive(text, code, error);
}

void WebSocket::Close(int code)
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->gateway) m_state->gateway->Close(code);
}

void WebSocket::Abort()
{
    std::shared_ptr<Gateway> gateway;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        ++m_state->generation;
        gateway = std::move(m_state->gateway);
    }
    if (gateway) gateway->Stop();
}
} // namespace Transport
