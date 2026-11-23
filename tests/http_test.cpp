#include <gtest/gtest.h>

#include "plgen/http.hpp"
#include "plgen/json.hpp"

using namespace plgen;

TEST(Http, ParsesGetWithQueryAndHeaders) {
  HttpRequest r;
  ASSERT_EQ(parse_request("GET /similar?song=abc&k=20 HTTP/1.1\r\nHost: x\r\nX-Thing:  y \r\n\r\n", r),
            ParseResult::kOk);
  EXPECT_EQ(r.method, "GET");
  EXPECT_EQ(r.path, "/similar");
  EXPECT_EQ(r.query.at("song"), "abc");
  EXPECT_EQ(r.query.at("k"), "20");
  EXPECT_EQ(r.headers.at("host"), "x");      // names are lowercased
  EXPECT_EQ(r.headers.at("x-thing"), "y");   // values are trimmed
}

TEST(Http, IncompleteUntilBlankLineAndFullBody) {
  HttpRequest r;
  EXPECT_EQ(parse_request("GET / HTTP/1.1\r\nHost: x\r\n", r), ParseResult::kIncomplete);
  const std::string post = "POST /playlist HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel";
  EXPECT_EQ(parse_request(post, r), ParseResult::kIncomplete);
  ASSERT_EQ(parse_request(post + "lo", r), ParseResult::kOk);
  EXPECT_EQ(r.body, "hello");
}

TEST(Http, RejectsMalformed) {
  HttpRequest r;
  EXPECT_EQ(parse_request("GARBAGE\r\n\r\n", r), ParseResult::kBad);
  EXPECT_EQ(parse_request("GET nopath HTTP/1.1\r\n\r\n", r), ParseResult::kBad);
  EXPECT_EQ(parse_request("GET / HTTP/1.1\r\nno colon here\r\n\r\n", r), ParseResult::kBad);
  EXPECT_EQ(parse_request("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", r), ParseResult::kBad);
}

TEST(Http, UrlDecoding) {
  EXPECT_EQ(url_decode("rainy%20day+study"), "rainy day study");
  EXPECT_EQ(url_decode("%C3%81ries"), "\xC3\x81ries");  // UTF-8 "Áries"
  EXPECT_EQ(url_decode("100%"), "100%");                 // invalid escape kept as-is
  EXPECT_EQ(url_decode("%zz"), "%zz");
}

TEST(Http, QueryStringEdgeCases) {
  const auto q = parse_query("a=1&&b=&c&a=2&q=somebody%20else");
  EXPECT_EQ(q.at("a"), "1");  // first value wins
  EXPECT_EQ(q.at("b"), "");
  EXPECT_EQ(q.at("c"), "");
  EXPECT_EQ(q.at("q"), "somebody else");
}

TEST(Http, SerializeSetsLengthAndStatus) {
  const std::string s = serialize({404, "application/json", "{}"});
  EXPECT_TRUE(s.starts_with("HTTP/1.1 404 Not Found\r\n"));
  EXPECT_NE(s.find("Content-Length: 2\r\n"), std::string::npos);
  EXPECT_TRUE(s.ends_with("\r\n\r\n{}"));
}

TEST(Json, EscapesSpecialCharacters) {
  EXPECT_EQ(json_string(R"(say "hi")"), R"("say \"hi\"")");
  EXPECT_EQ(json_string("a\\b\nc"), R"("a\\b\nc")");
  EXPECT_EQ(json_string(std::string("\x01", 1)), R"("\u0001")");
  EXPECT_EQ(json_string("Áries"), "\"Áries\"");  // UTF-8 untouched
}
