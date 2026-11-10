// net/json.h: the multiplayer messages' JSON reader and writer (types, escapes, surrogate pairs, invalid input) and the UTF-8 cut.
#include <cstdio>
#include <string>

#include "net/json.h"

using namespace oc;
using net::Json;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

bool rejects(const char* text) { return !net::parse_json(text).has_value(); }
}  // namespace

int main() {
  {
    std::string wide = "{";
    for (int i = 0; i < 300; ++i) wide += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
    CHECK(rejects((wide + "}").c_str()));
  }
  {
    const auto value = net::parse_json(R"( {"t":"pres","x":-120,"m":true,"s":null,"list":[1,2,{"a":"b"}],"f":2.6,"e":1e2} )");
    CHECK(value && value->is_object());
    CHECK(value->string_of("t") == "pres");
    CHECK(value->int_of("x") == -120);
    CHECK(value->get("m").as_bool());
    CHECK(value->get("s").is_null());
    CHECK(value->get("missing").is_null());
    CHECK(value->int_of("missing", 7) == 7);
    CHECK(value->get("list").items().size() == 3);
    CHECK(value->get("list").items()[2].string_of("a") == "b");
    CHECK(value->int_of("f") == 3);
    CHECK(value->int_of("e") == 100);
    CHECK(value->get("t").as_int(5) == 5);
  }
  {
    const auto value = net::parse_json(R"("a\"b\\c\/d\n\téあ")");
    CHECK(value && value->as_string() == "a\"b\\c/d\n\t\xC3\xA9\xE3\x81\x82");
    const auto pair = net::parse_json(R"("😀")");
    CHECK(pair && pair->as_string() == "\xF0\x9F\x98\x80");
    const auto lone_high = net::parse_json(R"("\ud83dx")");
    CHECK(lone_high && lone_high->as_string() == "\xEF\xBF\xBDx");
    const auto lone_low = net::parse_json(R"("\ude00")");
    CHECK(lone_low && lone_low->as_string() == "\xEF\xBF\xBD");
    const auto raw = net::parse_json("\"\xE2\x98\x85\"");
    CHECK(raw && raw->as_string() == "\xE2\x98\x85");
  }
  CHECK(rejects(""));
  CHECK(rejects("{"));
  CHECK(rejects("[1,]"));
  CHECK(rejects(R"({"a":1,})"));
  CHECK(rejects(R"({"a" 1})"));
  CHECK(rejects(R"("open)"));
  CHECK(rejects("\"tab\there\""));
  CHECK(rejects(R"("\x")"));
  CHECK(rejects(R"("\u12")"));
  CHECK(rejects("tru"));
  CHECK(rejects("1 2"));
  CHECK(rejects("-"));
  CHECK(rejects("1."));
  {
    std::string deep(65, '[');
    deep += std::string(65, ']');
    CHECK(rejects(deep.c_str()));
    std::string ok(64, '[');
    ok += std::string(64, ']');
    CHECK(!rejects(ok.c_str()));
  }
  {
    Json message = Json::object();
    message.set("t", "chat").set("n", 42).set("neg", -3).set("ok", true).set("none", nullptr);
    message.set("text", "say \"hi\"\\\n\x01\xE2\x98\x85");
    message.set("list", Json::array().push(1).push("two"));
    message.set("n", 43);
    CHECK(net::to_json(message) ==
          R"({"t":"chat","n":43,"neg":-3,"ok":true,"none":null,"text":"say \"hi\"\\\n\u0001)" "\xE2\x98\x85" R"(","list":[1,"two"]})");
    const auto back = net::parse_json(net::to_json(message));
    CHECK(back && back->string_of("text") == message.string_of("text"));
    CHECK(net::to_json(Json::object()) == "{}");
    CHECK(net::to_json(Json::array()) == "[]");
  }
  CHECK(net::to_json(Json("bad\xFF" "byte\xC3")) == "\"bad\xEF\xBF\xBD" "byte\xEF\xBF\xBD\"");
  CHECK(net::to_json(Json("\xED\xA0\x80")) == "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"");
  CHECK(net::utf8_truncate("a\xC3\xA9\xE3\x81\x82\xF0\x9F\x98\x80", 3) == "a\xC3\xA9\xE3\x81\x82");
  CHECK(net::utf8_truncate("abc", 10) == "abc");

  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("net json: all passed\n");
  return failures ? 1 : 0;
}
