// Exercise internal rate-limit parsing without creating a network connection.
#include "../src/Client.cpp"
#include <cstdio>

int main()
{
    const bool ok = Discord::ParseSeconds("1.25", 7) == 1.25
        && Discord::ParseSeconds("nan", 7) == 7
        && Discord::ParseSeconds("inf", 7) == 7
        && Discord::ParseSeconds("-1", 7) == 7
        && Discord::ParseSeconds("2junk", 7) == 7
        && Discord::ParseSeconds("1e300", 7) == 86400
        && Discord::ParseSeconds("", 7) == 7;
    if (!ok) std::puts("Rate-limit parsing regression");
    return ok ? 0 : 1;
}
