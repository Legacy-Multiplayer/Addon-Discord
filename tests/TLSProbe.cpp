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
