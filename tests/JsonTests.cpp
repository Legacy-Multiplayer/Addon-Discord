#include "Json.h"

#include <cstdio>
#include <string>

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* expression, int line)
{
    ++g_checks;

    if (!condition)
    {
        ++g_failures;
        std::printf("FAILED line %d: %s\n", line, expression);
    }
}

#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Throws(const char* text)
{
    try
    {
        Json::Parse(text);
    }
    catch (const std::exception&)
    {
        return true;
    }

    return false;
}

} // namespace

int main()
{
    {
        const auto value = Json::Parse(R"({"op":0,"t":"MESSAGE_CREATE","s":42,"d":{"id":"1234567890123456789","channel_id":"987","content":"Hi \"there\"\n","author":{"username":"Kavnn","bot":false},"mentions":[],"nonce":null,"score":1.5}})");
        CHECK(value["op"].Integer() == 0);
        CHECK(value["t"].String() == "MESSAGE_CREATE");
        CHECK(value["s"].Integer() == 42);
        const auto& data = value["d"];
        CHECK(data["id"].String() == "1234567890123456789");
        CHECK(data["content"].String() == "Hi \"there\"\n");
        CHECK(data["author"]["username"].String() == "Kavnn");
        CHECK(data["author"]["bot"].IsBool() && !data["author"]["bot"].Bool(true));
        CHECK(data["mentions"].IsArray() && data["mentions"].Items().empty());
        CHECK(data["nonce"].IsNull());
        CHECK(data["score"].Number() == 1.5);
        CHECK(data["missing"].IsNull());
    }

    {
        const auto value = Json::Parse(R"("\u00e9\u4e2d\ud83d\ude00")");
        CHECK(value.String() == "\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80");
    }

    {
        Json::Value message;
        message.Set("content", "line1\nline2 \"q\" \\ \t");
        message.Set("tts", false);
        message.Set("flags", 64);
        Json::Array embeds;
        Json::Value embed;
        embed.Set("title", "Status");
        embed.Set("color", int64_t{14886955});
        embeds.push_back(embed);
        message.Set("embeds", Json::Value(embeds));
        message.Set("allowed_mentions", Json::Value(Json::Object{{"parse", Json::Value(Json::Array{})}}));
        const auto text = Json::Dump(message);
        CHECK(text == R"({"content":"line1\nline2 \"q\" \\ \t","tts":false,"flags":64,"embeds":[{"title":"Status","color":14886955}],"allowed_mentions":{"parse":[]}})");
        const auto back = Json::Parse(text);
        CHECK(back["content"].String() == "line1\nline2 \"q\" \\ \t");
        CHECK(back["embeds"].Items()[0]["color"].Integer() == 14886955);
    }

    {
        CHECK(Json::SnakeToCamel("channel_id") == "channelId");
        CHECK(Json::SnakeToCamel("resume_gateway_url") == "resumeGatewayUrl");
        CHECK(Json::SnakeToCamel("id") == "id");
        CHECK(Json::SnakeToCamel("_private") == "_private");
        CHECK(Json::CamelToSnake("allowedMentions") == "allowed_mentions");
        CHECK(Json::CamelToSnake("customId") == "custom_id");
        CHECK(Json::CamelToSnake("content") == "content");
        CHECK(Json::CamelToSnake("already_snake") == "already_snake");
        CHECK(Json::CamelToSnake(Json::SnakeToCamel("communication_disabled_until")) == "communication_disabled_until");
    }

    {
        const auto value = Json::Parse(R"({"guild_id":"1","member":{"user":{"global_name":"Jims"},"joined_at":"x"},"data":{"options":[{"name":"player_name","value":"jims"}]}})");
        const auto camel = Json::RenameKeys(value, true);
        CHECK(camel["guildId"].String() == "1");
        CHECK(camel["member"]["user"]["globalName"].String() == "Jims");
        CHECK(camel["data"]["options"].Items()[0]["name"].String() == "player_name");
        const auto snake = Json::RenameKeys(camel, false);
        CHECK(snake["member"]["joined_at"].String() == "x");
    }

    {
        Json::Value object;
        object.Set("a", 1);
        object.Set("b", 2);
        object.Set("a", 3);
        object.Remove("b");
        CHECK(Json::Dump(object) == R"({"a":3})");
    }

    {
        CHECK(Json::Parse("-12").Integer() == -12);
        CHECK(Json::Parse("1e3").Number() == 1000.0);
        CHECK(Json::Parse("9223372036854775807").Integer() == 9223372036854775807LL);
        CHECK(Json::Parse(" [1, 2 ,3] ").Items().size() == 3);
        CHECK(Json::Parse("{}").IsObject());
    }

    CHECK(Throws(""));
    CHECK(Throws("{"));
    CHECK(Throws("[1,]"));
    CHECK(Throws("{\"a\" 1}"));
    CHECK(Throws("\"unterminated"));
    CHECK(Throws("tru"));
    CHECK(Throws("{} extra"));
    CHECK(Throws("\"\\x\""));

    std::printf("%d checks, %d failed\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
