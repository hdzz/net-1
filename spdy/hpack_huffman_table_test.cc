// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/hpack_huffman_table.h"

#include <bitset>
#include <string>

#include "base/logging.h"
#include "net/spdy/hpack_constants.h"
#include "net/spdy/hpack_input_stream.h"
#include "net/spdy/hpack_output_stream.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using base::StringPiece;
using std::string;
using testing::ElementsAre;
using testing::Pointwise;

namespace net {

namespace test {

typedef HpackHuffmanTable::DecodeEntry DecodeEntry;
typedef HpackHuffmanTable::DecodeTable DecodeTable;

class HpackHuffmanTablePeer {
 public:
  explicit HpackHuffmanTablePeer(const HpackHuffmanTable& table)
      : table_(table) { }

  const std::vector<uint32>& code_by_id() const {
    return table_.code_by_id_;
  }
  const std::vector<uint8>& length_by_id() const {
    return table_.length_by_id_;
  }
  const std::vector<DecodeTable>& decode_tables() const {
    return table_.decode_tables_;
  }
  uint16 failed_symbol_id() const {
    return table_.failed_symbol_id_;
  }
  std::vector<DecodeEntry> decode_entries(const DecodeTable& decode_table) {
    std::vector<DecodeEntry>::const_iterator begin =
        table_.decode_entries_.begin() + decode_table.entries_offset;
    return std::vector<DecodeEntry>(begin, begin + decode_table.size());
  }
  void DumpDecodeTable(const DecodeTable& table) {
    std::vector<DecodeEntry> entries = decode_entries(table);
    LOG(INFO) << "Table size " << (1 << table.indexed_length)
              << " prefix " << unsigned(table.prefix_length)
              << " indexed " << unsigned(table.indexed_length);
    size_t i = 0;
    while (i != table.size()) {
      const DecodeEntry& entry = entries[i];
      LOG(INFO) << i << ":"
                << " next_table " << unsigned(entry.next_table_index)
                << " length " << unsigned(entry.length)
                << " symbol " << unsigned(entry.symbol_id);
      size_t j = 1;
      for (; (i + j) != table.size(); j++) {
        const DecodeEntry& next = entries[i + j];
        if (next.next_table_index != entry.next_table_index ||
            next.length != entry.length ||
            next.symbol_id != entry.symbol_id)
          break;
      }
      if (j > 1) {
        LOG(INFO) << "  (repeats " << j << " times)";
      }
      i += j;
    }
  }

 private:
  const HpackHuffmanTable& table_;
};

namespace {

MATCHER(DecodeEntryEq, "") {
  const DecodeEntry& lhs = std::tr1::get<0>(arg);
  const DecodeEntry& rhs = std::tr1::get<1>(arg);
  return lhs.next_table_index == rhs.next_table_index &&
      lhs.length == rhs.length &&
      lhs.symbol_id == rhs.symbol_id;
}

uint32 bits32(const string& bitstring) {
  return std::bitset<32>(bitstring).to_ulong();
}
uint8 bits8(const string& bitstring) {
  return static_cast<uint8>(std::bitset<8>(bitstring).to_ulong());
}

TEST(HpackHuffmanTableTest, InitializeRequestCode) {
  HpackHuffmanTable table;
  std::vector<HpackHuffmanSymbol> code = HpackRequestHuffmanCode();
  EXPECT_TRUE(table.Initialize(&code[0], code.size()));
  EXPECT_TRUE(table.IsInitialized());
}

TEST(HpackHuffmanTableTest, InitializeResponseCode) {
  HpackHuffmanTable table;
  std::vector<HpackHuffmanSymbol> code = HpackResponseHuffmanCode();
  EXPECT_TRUE(table.Initialize(&code[0], code.size()));
  EXPECT_TRUE(table.IsInitialized());
}

TEST(HpackHuffmanTableTest, InitializeEdgeCases) {
  {
    // Verify eight symbols can be encoded with 3 bits per symbol.
    HpackHuffmanSymbol code[] = {
      {bits32("00000000000000000000000000000000"), 3, 0},
      {bits32("00100000000000000000000000000000"), 3, 1},
      {bits32("01000000000000000000000000000000"), 3, 2},
      {bits32("01100000000000000000000000000000"), 3, 3},
      {bits32("10000000000000000000000000000000"), 3, 4},
      {bits32("10100000000000000000000000000000"), 3, 5},
      {bits32("11000000000000000000000000000000"), 3, 6},
      {bits32("11100000000000000000000000000000"), 3, 7}};
    HpackHuffmanTable table;
    EXPECT_TRUE(table.Initialize(code, arraysize(code)));
  }
  {
    // But using 2 bits with one symbol overflows the code.
    HpackHuffmanSymbol code[] = {
      {bits32("01000000000000000000000000000000"), 3, 0},
      {bits32("01100000000000000000000000000000"), 3, 1},
      {bits32("00000000000000000000000000000000"), 2, 2},
      {bits32("10000000000000000000000000000000"), 3, 3},
      {bits32("10100000000000000000000000000000"), 3, 4},
      {bits32("11000000000000000000000000000000"), 3, 5},
      {bits32("11100000000000000000000000000000"), 3, 6},
      {bits32("00000000000000000000000000000000"), 3, 7}};  // Overflow.
    HpackHuffmanTable table;
    EXPECT_FALSE(table.Initialize(code, arraysize(code)));
    EXPECT_EQ(7, HpackHuffmanTablePeer(table).failed_symbol_id());
  }
  {
    // Verify four symbols can be encoded with incremental bits per symbol.
    HpackHuffmanSymbol code[] = {
      {bits32("00000000000000000000000000000000"), 1, 0},
      {bits32("10000000000000000000000000000000"), 2, 1},
      {bits32("11000000000000000000000000000000"), 3, 2},
      {bits32("11100000000000000000000000000000"), 4, 3}};
    HpackHuffmanTable table;
    EXPECT_TRUE(table.Initialize(code, arraysize(code)));
  }
  {
    // But repeating a length overflows the code.
    HpackHuffmanSymbol code[] = {
      {bits32("00000000000000000000000000000000"), 1, 0},
      {bits32("10000000000000000000000000000000"), 2, 1},
      {bits32("11000000000000000000000000000000"), 2, 2},
      {bits32("00000000000000000000000000000000"), 4, 3}};  // Overflow.
    HpackHuffmanTable table;
    EXPECT_FALSE(table.Initialize(code, arraysize(code)));
    EXPECT_EQ(3, HpackHuffmanTablePeer(table).failed_symbol_id());
  }
  {
    // Symbol IDs must be assigned sequentially with no gaps.
    HpackHuffmanSymbol code[] = {
      {bits32("00000000000000000000000000000000"), 1, 0},
      {bits32("10000000000000000000000000000000"), 2, 1},
      {bits32("11000000000000000000000000000000"), 3, 1},  // Repeat.
      {bits32("11100000000000000000000000000000"), 4, 3}};
    HpackHuffmanTable table;
    EXPECT_FALSE(table.Initialize(code, arraysize(code)));
    EXPECT_EQ(2, HpackHuffmanTablePeer(table).failed_symbol_id());
  }
  {
    // Canonical codes must begin with zero.
    HpackHuffmanSymbol code[] = {
      {bits32("10000000000000000000000000000000"), 4, 0},
      {bits32("10010000000000000000000000000000"), 4, 1},
      {bits32("10100000000000000000000000000000"), 4, 2},
      {bits32("10110000000000000000000000000000"), 4, 3}};
    HpackHuffmanTable table;
    EXPECT_FALSE(table.Initialize(code, arraysize(code)));
    EXPECT_EQ(0, HpackHuffmanTablePeer(table).failed_symbol_id());
  }
  {
    // Codes must match the expected canonical sequence.
    HpackHuffmanSymbol code[] = {
      {bits32("00000000000000000000000000000000"), 2, 0},
      {bits32("01000000000000000000000000000000"), 2, 1},
      {bits32("11000000000000000000000000000000"), 2, 2},  // Not canonical.
      {bits32("10000000000000000000000000000000"), 2, 3}};
    HpackHuffmanTable table;
    EXPECT_FALSE(table.Initialize(code, arraysize(code)));
    EXPECT_EQ(2, HpackHuffmanTablePeer(table).failed_symbol_id());
  }
}

TEST(HpackHuffmanTableTest, ValidateInternalsWithSmallCode) {
  HpackHuffmanSymbol code[] = {
    {bits32("01100000000000000000000000000000"), 4, 0},  // 3rd.
    {bits32("01110000000000000000000000000000"), 4, 1},  // 4th.
    {bits32("00000000000000000000000000000000"), 2, 2},  // 1st assigned code.
    {bits32("01000000000000000000000000000000"), 3, 3},  // 2nd.
    {bits32("10000000000000000000000000000000"), 5, 4},  // 5th.
    {bits32("10001000000000000000000000000000"), 5, 5},  // 6th.
    {bits32("10011000000000000000000000000000"), 6, 6},  // 8th.
    {bits32("10010000000000000000000000000000"), 5, 7}};  // 7th.
  HpackHuffmanTable table;
  EXPECT_TRUE(table.Initialize(code, arraysize(code)));

  HpackHuffmanTablePeer peer(table);
  EXPECT_THAT(peer.code_by_id(), ElementsAre(
      bits32("01100000000000000000000000000000"),
      bits32("01110000000000000000000000000000"),
      bits32("00000000000000000000000000000000"),
      bits32("01000000000000000000000000000000"),
      bits32("10000000000000000000000000000000"),
      bits32("10001000000000000000000000000000"),
      bits32("10011000000000000000000000000000"),
      bits32("10010000000000000000000000000000")));
  EXPECT_THAT(peer.length_by_id(), ElementsAre(
      4, 4, 2, 3, 5, 5, 6, 5));

  EXPECT_EQ(peer.decode_tables().size(), 1u);
  {
    std::vector<DecodeEntry> expected;
    expected.resize(128, DecodeEntry(0, 2, 2));  // Fills 128.
    expected.resize(192, DecodeEntry(0, 3, 3));  // Fills 64.
    expected.resize(224, DecodeEntry(0, 4, 0));  // Fills 32.
    expected.resize(256, DecodeEntry(0, 4, 1));  // Fills 32.
    expected.resize(272, DecodeEntry(0, 5, 4));  // Fills 16.
    expected.resize(288, DecodeEntry(0, 5, 5));  // Fills 16.
    expected.resize(304, DecodeEntry(0, 5, 7));  // Fills 16.
    expected.resize(312, DecodeEntry(0, 6, 6));  // Fills 8.
    expected.resize(512, DecodeEntry());  // Remainder is empty.

    EXPECT_THAT(peer.decode_entries(peer.decode_tables()[0]),
                Pointwise(DecodeEntryEq(), expected));
  }

  char input_storage[] = {2, 3, 2, 6, 7, 0};
  StringPiece input(input_storage, arraysize(input_storage));
  // By symbol: (2) 00 (3) 010 (2) 00 (6) 100110 (7) 10010 (0) 0110 (pad) 11.
  char expect_storage[] = {
    bits8("00010001"),
    bits8("00110100"),
    bits8("10011011")};
  StringPiece expect(expect_storage, arraysize(expect_storage));

  string buffer_in, buffer_out;
  HpackOutputStream output_stream(kuint32max);
  table.EncodeString(input, &output_stream);
  output_stream.TakeString(&buffer_in);
  EXPECT_EQ(buffer_in, expect);

  HpackInputStream input_stream(kuint32max, buffer_in);
  table.DecodeString(input.size(), &input_stream, &buffer_out);
  EXPECT_EQ(buffer_out, input);
}

TEST(HpackHuffmanTableTest, ValidateMultiLevelDecodeTables) {
  HpackHuffmanSymbol code[] = {
    {bits32("00000000000000000000000000000000"), 6, 0},
    {bits32("00000100000000000000000000000000"), 6, 1},
    {bits32("00001000000000000000000000000000"), 11, 2},
    {bits32("00001000001000000000000000000000"), 11, 3},
    {bits32("00001000010000000000000000000000"), 12, 4},
  };
  HpackHuffmanTable table;
  EXPECT_TRUE(table.Initialize(code, arraysize(code)));

  HpackHuffmanTablePeer peer(table);
  EXPECT_EQ(peer.decode_tables().size(), 2u);
  {
    std::vector<DecodeEntry> expected;
    expected.resize(8, DecodeEntry(0, 6, 0));  // Fills 8.
    expected.resize(16, DecodeEntry(0, 6, 1));  // Fills 8.
    expected.resize(17, DecodeEntry(1, 12, 0));  // Pointer. Fills 1.
    expected.resize(512, DecodeEntry());  // Remainder is empty.

    const DecodeTable& decode_table = peer.decode_tables()[0];
    EXPECT_EQ(decode_table.prefix_length, 0);
    EXPECT_EQ(decode_table.indexed_length, 9);
    EXPECT_THAT(peer.decode_entries(decode_table),
                Pointwise(DecodeEntryEq(), expected));
  }
  {
    std::vector<DecodeEntry> expected;
    expected.resize(2, DecodeEntry(1, 11, 2));  // Fills 2.
    expected.resize(4, DecodeEntry(1, 11, 3));  // Fills 2.
    expected.resize(5, DecodeEntry(1, 12, 4));  // Fills 1.
    expected.resize(8, DecodeEntry());  // Remainder is empty.

    const DecodeTable& decode_table = peer.decode_tables()[1];
    EXPECT_EQ(decode_table.prefix_length, 9);
    EXPECT_EQ(decode_table.indexed_length, 3);
    EXPECT_THAT(peer.decode_entries(decode_table),
                Pointwise(DecodeEntryEq(), expected));
  }
}

TEST(HpackHuffmanTableTest, DecodeWithBadInput) {
  // Use the same code as ValidateInternalsWithSmallCode above.
  HpackHuffmanSymbol code[] = {
    {bits32("01100000000000000000000000000000"), 4, 0},
    {bits32("01110000000000000000000000000000"), 4, 1},
    {bits32("00000000000000000000000000000000"), 2, 2},
    {bits32("01000000000000000000000000000000"), 3, 3},
    {bits32("10000000000000000000000000000000"), 5, 4},
    {bits32("10001000000000000000000000000000"), 5, 5},
    {bits32("10011000000000000000000000000000"), 6, 6},
    {bits32("10010000000000000000000000000000"), 5, 7}};
  HpackHuffmanTable table;
  EXPECT_TRUE(table.Initialize(code, arraysize(code)));
  {
    // This example works: (2) 00 (3) 010 (2) 00 (6) 100110 (pad) 111.
    char input_storage[] = {bits8("00010001"), bits8("00110111")};
    StringPiece input(input_storage, arraysize(input_storage));

    string buffer;
    HpackInputStream input_stream(kuint32max, input);
    EXPECT_TRUE(table.DecodeString(4, &input_stream, &buffer));
    EXPECT_EQ(buffer, "\x02\x03\x02\x06");
  }
  {
    // (2) 00 (3) 010 (2) 00 (too-large) 101000 (pad) 111.
    char input_storage[] = {bits8("00010001"), bits8("01000111")};
    StringPiece input(input_storage, arraysize(input_storage));

    string buffer;
    HpackInputStream input_stream(kuint32max, input);
    EXPECT_FALSE(table.DecodeString(4, &input_stream, &buffer));
    EXPECT_EQ(buffer, "\x02\x03\x02");
  }
  {
    // Would work, save for insufficient input.
    char input_storage[] = {bits8("00010001"), bits8("00110111")};
    StringPiece input(input_storage, arraysize(input_storage));

    string buffer;
    HpackInputStream input_stream(kuint32max, input);
    EXPECT_FALSE(table.DecodeString(6, &input_stream, &buffer));
    EXPECT_EQ(buffer, "\x02\x03\x02\x06");
  }
}

TEST(HpackHuffmanTableTest, SpecRequestExamples) {
  HpackHuffmanTable table;
  {
    std::vector<HpackHuffmanSymbol> code = HpackRequestHuffmanCode();
    EXPECT_TRUE(table.Initialize(&code[0], code.size()));
  }
  string buffer;
  string test_table[] = {
    "\xdb\x6d\x88\x3e\x68\xd1\xcb\x12\x25\xba\x7f",
    "www.example.com",
    "\x63\x65\x4a\x13\x98\xff",
    "no-cache",
    "\x4e\xb0\x8b\x74\x97\x90\xfa\x7f",
    "custom-key",
    "\x4e\xb0\x8b\x74\x97\x9a\x17\xa8\xff",
    "custom-value",
  };
  // Round-trip each test example.
  for (size_t i = 0; i != arraysize(test_table); i += 2) {
    const string& encodedFixture(test_table[i]);
    const string& decodedFixture(test_table[i+1]);
    HpackInputStream input_stream(kuint32max, encodedFixture);
    HpackOutputStream output_stream(kuint32max);

    table.DecodeString(decodedFixture.size(), &input_stream, &buffer);
    EXPECT_EQ(decodedFixture, buffer);
    table.EncodeString(decodedFixture, &output_stream);
    output_stream.TakeString(&buffer);
    EXPECT_EQ(encodedFixture, buffer);
  }
}

TEST(HpackHuffmanTableTest, SpecResponseExamples) {
  HpackHuffmanTable table;
  {
    std::vector<HpackHuffmanSymbol> code = HpackResponseHuffmanCode();
    EXPECT_TRUE(table.Initialize(&code[0], code.size()));
  }
  string buffer;
  string test_table[] = {
    "\x40\x9f",
    "302",
    "\xc3\x1b\x39\xbf\x38\x7f",
    "private",
    "\xa2\xfb\xa2\x03\x20\xf2\xab\x30\x31\x24\x01"
        "\x8b\x49\x0d\x32\x09\xe8\x77",
    "Mon, 21 Oct 2013 20:13:21 GMT",
    "\xe3\x9e\x78\x64\xdd\x7a\xfd\x3d\x3d\x24\x87"
        "\x47\xdb\x87\x28\x49\x55\xf6\xff",
    "https://www.example.com",
    "\xdf\x7d\xfb\x36\xd3\xd9\xe1\xfc\xfc\x3f\xaf"
        "\xe7\xab\xfc\xfe\xfc\xbf\xaf\x3e\xdf\x2f"
        "\x97\x7f\xd3\x6f\xf7\xfd\x79\xf6\xf9\x77"
        "\xfd\x3d\xe1\x6b\xfa\x46\xfe\x10\xd8\x89"
        "\x44\x7d\xe1\xce\x18\xe5\x65\xf7\x6c\x2f",
    "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1",
  };
  // Round-trip each test example.
  for (size_t i = 0; i != arraysize(test_table); i += 2) {
    const string& encodedFixture(test_table[i]);
    const string& decodedFixture(test_table[i+1]);
    HpackInputStream input_stream(kuint32max, encodedFixture);
    HpackOutputStream output_stream(kuint32max);

    table.DecodeString(decodedFixture.size(), &input_stream, &buffer);
    EXPECT_EQ(decodedFixture, buffer);
    table.EncodeString(decodedFixture, &output_stream);
    output_stream.TakeString(&buffer);
    EXPECT_EQ(encodedFixture, buffer);
  }
}

TEST(HpackHuffmanTableTest, RoundTripIndvidualSymbols) {
  HpackHuffmanTable table;
  {
    std::vector<HpackHuffmanSymbol> code = HpackRequestHuffmanCode();
    EXPECT_TRUE(table.Initialize(&code[0], code.size()));
  }
  for (size_t i = 0; i != 256; i++) {
    char c = static_cast<char>(i);
    char storage[3] = {c, c, c};
    StringPiece input(storage, arraysize(storage));

    string buffer_in, buffer_out;
    HpackOutputStream output_stream(kuint32max);
    table.EncodeString(input, &output_stream);
    output_stream.TakeString(&buffer_in);

    HpackInputStream input_stream(kuint32max, buffer_in);
    table.DecodeString(input.size(), &input_stream, &buffer_out);
    EXPECT_EQ(input, buffer_out);
  }
}

TEST(HpackHuffmanTableTest, RoundTripSymbolSequence) {
  HpackHuffmanTable table;
  {
    std::vector<HpackHuffmanSymbol> code = HpackResponseHuffmanCode();
    EXPECT_TRUE(table.Initialize(&code[0], code.size()));
  }
  char storage[512];
  for (size_t i = 0; i != 256; i++) {
    storage[i] = static_cast<char>(i);
    storage[511 - i] = static_cast<char>(i);
  }
  StringPiece input(storage, arraysize(storage));

  string buffer_in, buffer_out;
  HpackOutputStream output_stream(kuint32max);
  table.EncodeString(input, &output_stream);
  output_stream.TakeString(&buffer_in);

  HpackInputStream input_stream(kuint32max, buffer_in);
  table.DecodeString(input.size(), &input_stream, &buffer_out);
  EXPECT_EQ(input, buffer_out);
}

}  // namespace

}  // namespace test

}  // namespace net
