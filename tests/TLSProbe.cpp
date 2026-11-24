#include "Transport.h"
#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 4) return 1;
    Transport::Https https;
    Transport::Response response;
    const std::string mode = argv[3];
    const auto start = std::chrono::steady_clock::now();
    if (mode == "websocket")
    {
        Transport::WebSocket socket;
        for (int pass = 0; pass < 2; ++pass)
        {
            std::string error, text;
            int code = 0;
            if (!socket.Connect(argv[1], argv[2], error)
                || socket.Receive(text, code, error) != Transport::Frame::Text || text != "hello"
                || !socket.Send("client")
                || socket.Receive(text, code, error) != Transport::Frame::Text || text != "ack")
            {
                std::printf("WebSocket fixture failed: %s\n", error.c_str());
                return 1;
            }
            const auto cancelStart = std::chrono::steady_clock::now();
            if (pass == 0)
            {
                std::thread reader([&]() { socket.Receive(text, code, error); });
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                socket.Abort();
                reader.join();
            }
            else
            {
                socket.Close(1000);
                const auto frame = socket.Receive(text, code, error);
                if (frame != Transport::Frame::Closed || code != 1000)
                {
                    std::printf("WebSocket close failed: code %d, %s\n", code, error.c_str());
                    return 1;
                }
                socket.Abort();
            }
            if (std::chrono::steady_clock::now() - cancelStart > std::chrono::seconds(3)) return 1;
        }
        return 0;
    }
    if (mode == "cancel")
    {
        std::thread worker([&]() { response = https.Request(argv[1], "GET", argv[2], {}, ""); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        https.Abort();
        worker.join();
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) return 1;
    }
    else response = https.Request(argv[1], "GET", argv[2], {}, "");
    if (mode == "ok")
    {
        if (response.ok && response.status == 200 && response.body == "ok"
            && response.headers["x-fixture"] == "yes") return 0;
        std::printf("HTTPS fixture failed: %s\n", response.error.c_str());
        return 1;
    }
    if (!response.ok && !response.error.empty()) return 0;
    std::puts("HTTPS fixture unexpectedly accepted a failure case");
    return 1;
}
