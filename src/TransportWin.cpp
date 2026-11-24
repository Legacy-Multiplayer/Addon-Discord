#include "Transport.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <vector>

namespace Transport
{
namespace
{
std::wstring Wide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring output(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), length);
    return output;
}

std::string Narrow(const wchar_t* text, size_t length)
{
    if (!text || !length)
    {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), output.data(), size, nullptr, nullptr);
    return output;
}

std::string LastError(const char* where)
{
    return std::string(where) + " failed (" + std::to_string(GetLastError()) + ")";
}

HINTERNET OpenSession()
{
    HINTERNET session = WinHttpOpen(L"Legacy-Addon-Discord/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!session)
    {
        session = WinHttpOpen(L"Legacy-Addon-Discord/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }

    if (session)
    {
        WinHttpSetTimeouts(session, 8000, 8000, 15000, 20000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }

    return session;
}

} // namespace

struct Https::State
{
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    std::string host;
    std::mutex mutex;
    HINTERNET active = nullptr;
    bool aborted = false;
};

Https::Https() : m_state(std::make_unique<State>())
{
}

Https::~Https()
{
    Abort();

    if (m_state->connection)
    {
        WinHttpCloseHandle(m_state->connection);
    }

    if (m_state->session)
    {
        WinHttpCloseHandle(m_state->session);
    }
}

void Https::Abort()
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->aborted = true;

    if (m_state->active)
    {
        WinHttpCloseHandle(m_state->active);
        m_state->active = nullptr;
    }
}

Response Https::Request(const std::string& host, const std::string& method, const std::string& path,
    const std::map<std::string, std::string>& headers, const std::string& body)
{
    Response response;
    auto& state = *m_state;

    {
        std::lock_guard<std::mutex> lock(state.mutex);

        if (state.aborted)
        {
            response.error = "Request aborted";
            return response;
        }
    }

    if (!state.session)
    {
        state.session = OpenSession();

        if (!state.session)
        {
            response.error = LastError("WinHttpOpen");
            return response;
        }
    }

    if (!state.connection || state.host != host)
    {
        if (state.connection)
        {
            WinHttpCloseHandle(state.connection);
        }

        state.connection = WinHttpConnect(state.session, Wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        state.host = host;

        if (!state.connection)
        {
            response.error = LastError("WinHttpConnect");
            return response;
        }
    }

    HINTERNET request = WinHttpOpenRequest(state.connection, Wide(method).c_str(), Wide(path).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    if (!request)
    {
        response.error = LastError("WinHttpOpenRequest");
        return response;
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);

        if (state.aborted)
        {
            WinHttpCloseHandle(request);
            response.error = "Request aborted";
            return response;
        }

        state.active = request;
    }

    const auto finish = [&state, &request]()
    {
        std::lock_guard<std::mutex> lock(state.mutex);

        if (state.active == request)
        {
            WinHttpCloseHandle(request);
            state.active = nullptr;
        }
    };

    std::wstring headerText;

    for (const auto& header : headers)
    {
        headerText += Wide(header.first + ": " + header.second + "\r\n");
    }

    const BOOL sent = WinHttpSendRequest(request, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
        headerText.empty() ? 0 : static_cast<DWORD>(-1L), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        response.error = LastError("HTTPS request");
        finish();
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);

    DWORD rawSize = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
        &rawSize, WINHTTP_NO_HEADER_INDEX);

    if (rawSize)
    {
        std::vector<wchar_t> raw(rawSize / sizeof(wchar_t) + 1);

        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &rawSize,
                WINHTTP_NO_HEADER_INDEX))
        {
            const auto text = Narrow(raw.data(), rawSize / sizeof(wchar_t));
            size_t start = 0;

            while (start < text.size())
            {
                size_t end = text.find("\r\n", start);

                if (end == std::string::npos)
                {
                    end = text.size();
                }

                const auto line = text.substr(start, end - start);
                const auto colon = line.find(':');

                if (colon != std::string::npos)
                {
                    std::string key = line.substr(0, colon);
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character)
                    {
                        return static_cast<char>(std::tolower(character));
                    });
                    auto value = line.substr(colon + 1);
                    value.erase(0, value.find_first_not_of(' '));
                    response.headers[key] = value;
                }

                start = end + 2;
            }
        }
    }

    while (true)
    {
        DWORD available = 0;

        if (!WinHttpQueryDataAvailable(request, &available))
        {
            response.error = LastError("WinHttpQueryDataAvailable");
            finish();
            return response;
        }

        if (!available)
        {
            break;
        }

        if (response.body.size() + available > 8 * 1024 * 1024)
        {
            response.error = "Response exceeds 8 MiB";
            finish();
            return response;
        }

        const size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;

        if (!WinHttpReadData(request, response.body.data() + offset, available, &read))
        {
            response.error = LastError("WinHttpReadData");
            finish();
            return response;
        }

        response.body.resize(offset + read);
    }

    finish();
    response.ok = true;
    return response;
}

struct WebSocket::State
{
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET socket = nullptr;
    std::mutex sendMutex;
    std::mutex handleMutex;
};

WebSocket::WebSocket() : m_state(std::make_unique<State>())
{
}

WebSocket::~WebSocket()
{
    Abort();

    if (m_state->connection)
    {
        WinHttpCloseHandle(m_state->connection);
    }

    if (m_state->session)
    {
        WinHttpCloseHandle(m_state->session);
    }
}

bool WebSocket::Connect(const std::string& host, const std::string& path, std::string& error)
{
    auto& state = *m_state;
    Abort();

    if (!state.session)
    {
        state.session = OpenSession();

        if (!state.session)
        {
            error = LastError("WinHttpOpen");
            return false;
        }
    }

    if (state.connection)
    {
        WinHttpCloseHandle(state.connection);
    }

    state.connection = WinHttpConnect(state.session, Wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (!state.connection)
    {
        error = LastError("WinHttpConnect");
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(state.connection, L"GET", Wide(path).c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    if (!request)
    {
        error = LastError("WinHttpOpenRequest");
        return false;
    }

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
    {
        error = LastError("WebSocket handshake");
        WinHttpCloseHandle(request);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (status != 101)
    {
        error = "WebSocket upgrade refused with HTTP " + std::to_string(status);
        WinHttpCloseHandle(request);
        return false;
    }

    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);

    if (!socket)
    {
        error = LastError("WinHttpWebSocketCompleteUpgrade");
        return false;
    }

    DWORD keepAlive = 0;
    WinHttpSetOption(socket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepAlive, sizeof(keepAlive));

    std::lock_guard<std::mutex> lock(state.handleMutex);
    state.socket = socket;
    return true;
}

bool WebSocket::Send(const std::string& text)
{
    auto& state = *m_state;
    std::lock_guard<std::mutex> sendLock(state.sendMutex);
    HINTERNET socket = nullptr;

    {
        std::lock_guard<std::mutex> lock(state.handleMutex);
        socket = state.socket;
    }

    if (!socket)
    {
        return false;
    }

    return WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(text.data()),
               static_cast<DWORD>(text.size())) == NO_ERROR;
}

Frame WebSocket::Receive(std::string& text, int& closeCode, std::string& closeReason)
{
    auto& state = *m_state;
    text.clear();
    closeCode = 0;
    closeReason.clear();
    std::vector<char> buffer(64 * 1024);

    while (true)
    {
        HINTERNET socket = nullptr;

        {
            std::lock_guard<std::mutex> lock(state.handleMutex);
            socket = state.socket;
        }

        if (!socket)
        {
            return Frame::Failed;
        }

        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        const DWORD result = WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);

        if (result != NO_ERROR)
        {
            closeReason = "Receive failed (" + std::to_string(result) + ")";
            return Frame::Failed;
        }

        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
        {
            USHORT status = 0;
            char reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH] = {};
            DWORD consumed = 0;

            if (WinHttpWebSocketQueryCloseStatus(socket, &status, reason, sizeof(reason), &consumed) == NO_ERROR)
            {
                closeCode = status;
                closeReason.assign(reason, consumed);
            }

            return Frame::Closed;
        }

        if (text.size() + read > 32 * 1024 * 1024)
        {
            closeReason = "Gateway message exceeds 32 MiB";
            return Frame::Failed;
        }

        text.append(buffer.data(), read);

        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
        {
            return Frame::Text;
        }
    }
}

void WebSocket::Close(int code)
{
    auto& state = *m_state;
    HINTERNET socket = nullptr;

    {
        std::lock_guard<std::mutex> lock(state.handleMutex);
        socket = state.socket;
    }

    if (socket)
    {
        std::lock_guard<std::mutex> sendLock(state.sendMutex);
        WinHttpWebSocketShutdown(socket, static_cast<USHORT>(code), nullptr, 0);
    }
}

void WebSocket::Abort()
{
    auto& state = *m_state;
    HINTERNET socket = nullptr;

    {
        std::lock_guard<std::mutex> lock(state.handleMutex);
        socket = state.socket;
        state.socket = nullptr;
    }

    if (socket)
    {
        WinHttpCloseHandle(socket);
    }
}

} // namespace Transport
