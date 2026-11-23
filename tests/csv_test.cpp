#include <gtest/gtest.h>

#include <sstream>

#include "plgen/csv.hpp"

using plgen::CsvHeader;
using plgen::CsvReader;
using plgen::split_csv_record;
using Fields = std::vector<std::string>;

TEST(Csv, PlainFields) { EXPECT_EQ(split_csv_record("a,b,c"), (Fields{"a", "b", "c"})); }

TEST(Csv, QuotedFieldWithComma) {
  EXPECT_EQ(split_csv_record(R"("Hello, World",x)"), (Fields{"Hello, World", "x"}));
}

TEST(Csv, EscapedQuote) {
  EXPECT_EQ(split_csv_record(R"("say ""hi""",y)"), (Fields{R"(say "hi")", "y"}));
}

TEST(Csv, EmptyFields) {
  EXPECT_EQ(split_csv_record(",,"), (Fields{"", "", ""}));
  EXPECT_EQ(split_csv_record(R"("",a)"), (Fields{"", "a"}));
}

TEST(Csv, StripsCarriageReturn) { EXPECT_EQ(split_csv_record("a,b\r"), (Fields{"a", "b"})); }

TEST(Csv, QuotedFieldSpanningLines) {
  std::istringstream in("\"line one\nline two\",x\nnext,row\n");
  CsvReader reader(in);
  Fields f;
  ASSERT_TRUE(reader.next(f));
  EXPECT_EQ(f, (Fields{"line one\nline two", "x"}));
  ASSERT_TRUE(reader.next(f));
  EXPECT_EQ(f, (Fields{"next", "row"}));
  EXPECT_EQ(reader.line_number(), 3u);
  EXPECT_FALSE(reader.next(f));
}

TEST(Csv, UnterminatedQuoteThrows) {
  std::istringstream in("\"never closed,x\n");
  CsvReader reader(in);
  Fields f;
  EXPECT_THROW(reader.next(f), std::runtime_error);
}

TEST(CsvHeader, LooksUpByNameAndStripsBom) {
  const CsvHeader h(Fields{"\xEF\xBB\xBFtrack_id", "tempo"});
  EXPECT_EQ(h.index_of("track_id"), 0u);
  EXPECT_EQ(h.index_of("tempo"), 1u);
  EXPECT_THROW((void)h.index_of("missing"), std::runtime_error);
}
