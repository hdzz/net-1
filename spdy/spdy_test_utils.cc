// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/spdy_test_utils.h"

#include <algorithm>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace test {

std::string HexDumpWithMarks(const unsigned char* data, int length,
                        const bool* marks, int mark_length) {
  static const char kHexChars[] = "0123456789abcdef";
  static const int kColumns = 4;

  const int kSizeLimit = 1024;
  if (length > kSizeLimit || mark_length > kSizeLimit) {
    LOG(ERROR) << "Only dumping first " << kSizeLimit << " bytes.";
    length = std::min(length, kSizeLimit);
    mark_length = std::min(mark_length, kSizeLimit);
  }

  std::string hex;
  for (const unsigned char* row = data; length > 0;
       row += kColumns, length -= kColumns) {
    for (const unsigned char *p = row; p < row + 4; ++p) {
      if (p < row + length) {
        const bool mark =
            (marks && (p - data) < mark_length && marks[p - data]);
        hex += mark ? '*' : ' ';
        hex += kHexChars[(*p & 0xf0) >> 4];
        hex += kHexChars[*p & 0x0f];
        hex += mark ? '*' : ' ';
      } else {
        hex += "    ";
      }
    }
    hex = hex + "  ";

    for (const unsigned char *p = row; p < row + 4 && p < row + length; ++p)
      hex += (*p >= 0x20 && *p <= 0x7f) ? (*p) : '.';

    hex = hex + '\n';
  }
  return hex;
}

void CompareCharArraysWithHexError(
    const std::string& description,
    const unsigned char* actual,
    const int actual_len,
    const unsigned char* expected,
    const int expected_len) {
  const int min_len = std::min(actual_len, expected_len);
  const int max_len = std::max(actual_len, expected_len);
  scoped_array<bool> marks(new bool[max_len]);
  bool identical = (actual_len == expected_len);
  for (int i = 0; i < min_len; ++i) {
    if (actual[i] != expected[i]) {
      marks[i] = true;
      identical = false;
    } else {
      marks[i] = false;
    }
  }
  for (int i = min_len; i < max_len; ++i) {
    marks[i] = true;
  }
  if (identical) return;
  ADD_FAILURE()
      << "Description:\n"
      << description
      << "\n\nExpected:\n"
      << HexDumpWithMarks(expected, expected_len, marks.get(), max_len)
      << "\nActual:\n"
      << HexDumpWithMarks(actual, actual_len, marks.get(), max_len);
}

void SetFrameFlags(SpdyFrame* frame, uint8 flags, int spdy_version) {
  switch (spdy_version) {
    case 2:
    case 3:
      frame->data()[4] = flags;
      break;
    default:
      LOG(FATAL) << "Unsupported SPDY version.";
  }
}

void SetFrameLength(SpdyFrame* frame, size_t length, int spdy_version) {
  switch (spdy_version) {
    case 2:
    case 3:
      CHECK_EQ(0u, length & ~kLengthMask);
      {
        int32 wire_length = htonl(length);
        // The length field in SPDY 2 and 3 is a 24-bit (3B) integer starting at
        // offset 5.
        memcpy(frame->data() + 5, reinterpret_cast<char*>(&wire_length) + 1, 3);
      }
      break;
    default:
      LOG(FATAL) << "Unsupported SPDY version.";
  }
}


}  // namespace test

}  // namespace net
