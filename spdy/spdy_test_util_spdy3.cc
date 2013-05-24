// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/spdy_test_util_spdy3.h"

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/string_number_conversions.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/spdy/buffered_spdy_framer.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/spdy/spdy_session.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace test_spdy3 {

namespace {

// Parses a URL into the scheme, host, and path components required for a
// SPDY request.
void ParseUrl(const char* const url, std::string* scheme, std::string* host,
              std::string* path) {
  GURL gurl(url);
  path->assign(gurl.PathForRequest());
  scheme->assign(gurl.scheme());
  host->assign(gurl.host());
  if (gurl.has_port()) {
    host->append(":");
    host->append(gurl.port());
  }
}

scoped_ptr<SpdyHeaderBlock> ConstructGetHeaderBlock(base::StringPiece url) {
  SpdyTestUtil util(kProtoSPDY3);
  return util.ConstructGetHeaderBlock(url);
}

scoped_ptr<SpdyHeaderBlock> ConstructPostHeaderBlock(base::StringPiece url,
                                                     int64 content_length) {
  SpdyTestUtil util(kProtoSPDY3);
  return util.ConstructPostHeaderBlock(url, content_length);
}

// Construct a SPDY frame. |spdy_version| must be SPDY3 or SPDY4.
SpdyFrame* ConstructSpdyFrameWithVersion(SpdyMajorVersion spdy_version,
                                         const SpdyHeaderInfo& header_info,
                                         scoped_ptr<SpdyHeaderBlock> headers) {
  DCHECK(spdy_version == SPDY3 || spdy_version == SPDY4);
  SpdyTestUtil util(NextProtoFromSpdyVersion(spdy_version));
  return util.ConstructSpdyFrame(header_info, headers.Pass());
}

// Construct a SPDY frame.
SpdyFrame* ConstructSpdyFrame(const SpdyHeaderInfo& header_info,
                              scoped_ptr<SpdyHeaderBlock> headers) {
  SpdyTestUtil util(kProtoSPDY3);
  return util.ConstructSpdyFrame(header_info, headers.Pass());
}

// Construct a SPDY control frame. |spdy_version| must be
// kSpdyVersion3 or kSpdyVersion4.
SpdyFrame* ConstructSpdyControlFrameWithVersion(
    SpdyMajorVersion spdy_verson,
    const char* const extra_headers[],
    int extra_header_count,
    bool compressed,
    SpdyStreamId stream_id,
    RequestPriority request_priority,
    SpdyFrameType type,
    SpdyControlFlags flags,
    const char* const* kHeaders,
    int kHeadersSize,
    SpdyStreamId associated_stream_id) {
  SpdyTestUtil util(NextProtoFromSpdyVersion(spdy_verson));
  return util.ConstructSpdyControlFrame(extra_headers,
                                        extra_header_count,
                                        compressed,
                                        stream_id,
                                        request_priority,
                                        type,
                                        flags,
                                        kHeaders,
                                        kHeadersSize,
                                        associated_stream_id);
}

// Construct a generic SPDY control frame with an associated stream ID.
SpdyFrame* ConstructSpdyControlFrame(const char* const extra_headers[],
                                     int extra_header_count,
                                     bool compressed,
                                     SpdyStreamId stream_id,
                                     RequestPriority request_priority,
                                     SpdyFrameType type,
                                     SpdyControlFlags flags,
                                     const char* const* kHeaders,
                                     int kHeadersSize,
                                     SpdyStreamId associated_stream_id) {
  SpdyTestUtil util(kProtoSPDY3);
  return util.ConstructSpdyControlFrame(extra_headers,
                                        extra_header_count,
                                        compressed,
                                        stream_id,
                                        request_priority,
                                        type,
                                        flags,
                                        kHeaders,
                                        kHeadersSize,
                                        associated_stream_id);
}

}  // namespace

SpdyFrame* ConstructSpdyPush(const char* const extra_headers[],
                             int extra_header_count,
                             int stream_id,
                             int associated_stream_id) {
  const char* const kStandardPushHeaders[] = {
    "hello", "bye",
    ":status",  "200",
    ":version", "HTTP/1.1"
  };
  return ConstructSpdyControlFrame(extra_headers,
                                   extra_header_count,
                                   false,
                                   stream_id,
                                   LOWEST,
                                   SYN_STREAM,
                                   CONTROL_FLAG_NONE,
                                   kStandardPushHeaders,
                                   arraysize(kStandardPushHeaders),
                                   associated_stream_id);
}

SpdyFrame* ConstructSpdyPush(const char* const extra_headers[],
                             int extra_header_count,
                             int stream_id,
                             int associated_stream_id,
                             const char* url) {
  std::string scheme, host, path;
  ParseUrl(url, &scheme, &host, &path);
  const char* const headers[] = {
    "hello",    "bye",
    ":status",  "200 OK",
    ":version", "HTTP/1.1",
    ":path",    path.c_str(),
    ":host",    host.c_str(),
    ":scheme",  scheme.c_str(),
  };
  return ConstructSpdyControlFrame(extra_headers,
                                   extra_header_count,
                                   false,
                                   stream_id,
                                   LOWEST,
                                   SYN_STREAM,
                                   CONTROL_FLAG_NONE,
                                   headers,
                                   arraysize(headers),
                                   associated_stream_id);

}
SpdyFrame* ConstructSpdyPush(const char* const extra_headers[],
                             int extra_header_count,
                             int stream_id,
                             int associated_stream_id,
                             const char* url,
                             const char* status,
                             const char* location) {
  std::string scheme, host, path;
  ParseUrl(url, &scheme, &host, &path);
  const char* const headers[] = {
    "hello",    "bye",
    ":status",  status,
    "location", location,
    ":path",    path.c_str(),
    ":host",    host.c_str(),
    ":scheme",  scheme.c_str(),
    ":version", "HTTP/1.1"
  };
  return ConstructSpdyControlFrame(extra_headers,
                                   extra_header_count,
                                   false,
                                   stream_id,
                                   LOWEST,
                                   SYN_STREAM,
                                   CONTROL_FLAG_NONE,
                                   headers,
                                   arraysize(headers),
                                   associated_stream_id);
}

SpdyFrame* ConstructSpdyPushHeaders(int stream_id,
                                    const char* const extra_headers[],
                                    int extra_header_count) {
  const char* const kStandardGetHeaders[] = {
    ":status", "200 OK",
    ":version", "HTTP/1.1"
  };
  return ConstructSpdyControlFrame(extra_headers,
                                   extra_header_count,
                                   false,
                                   stream_id,
                                   LOWEST,
                                   HEADERS,
                                   CONTROL_FLAG_NONE,
                                   kStandardGetHeaders,
                                   arraysize(kStandardGetHeaders),
                                   0);
}

SpdyFrame* ConstructSpdySynReplyError(const char* const status,
                                      const char* const* const extra_headers,
                                      int extra_header_count,
                                      int stream_id) {
  const char* const kStandardGetHeaders[] = {
    "hello", "bye",
    ":status",  status,
    ":version", "HTTP/1.1"
  };
  return ConstructSpdyControlFrame(extra_headers,
                                   extra_header_count,
                                   false,
                                   stream_id,
                                   LOWEST,
                                   SYN_REPLY,
                                   CONTROL_FLAG_NONE,
                                   kStandardGetHeaders,
                                   arraysize(kStandardGetHeaders),
                                   0);
}

SpdyFrame* ConstructSpdyGetSynReplyRedirect(int stream_id) {
  static const char* const kExtraHeaders[] = {
    "location", "http://www.foo.com/index.php",
  };
  return ConstructSpdySynReplyError("301 Moved Permanently", kExtraHeaders,
                                    arraysize(kExtraHeaders)/2, stream_id);
}

SpdyFrame* ConstructSpdySynReplyError(int stream_id) {
  return ConstructSpdySynReplyError("500 Internal Server Error", NULL, 0, 1);
}

SpdyFrame* ConstructSpdyGetSynReply(const char* const extra_headers[],
                                    int extra_header_count,
                                    int stream_id) {
  static const char* const kStandardGetHeaders[] = {
    "hello", "bye",
    ":status", "200",
    ":version", "HTTP/1.1"
  };
  return ConstructSpdyControlFrame(extra_headers,
                                   extra_header_count,
                                   false,
                                   stream_id,
                                   LOWEST,
                                   SYN_REPLY,
                                   CONTROL_FLAG_NONE,
                                   kStandardGetHeaders,
                                   arraysize(kStandardGetHeaders),
                                   0);
}

SpdyFrame* ConstructSpdyPost(const char* url,
                             SpdyStreamId stream_id,
                             int64 content_length,
                             RequestPriority priority,
                             const char* const extra_headers[],
                             int extra_header_count) {
  const SpdyHeaderInfo kSynStartHeader = {
    SYN_STREAM,                   // Kind = Syn
    stream_id,                    // Stream ID
    0,                            // Associated stream ID
    ConvertRequestPriorityToSpdyPriority(priority, SPDY3),
                                  // Priority
    0,                            // Credential Slot
    CONTROL_FLAG_NONE,            // Control Flags
    false,                        // Compressed
    RST_STREAM_INVALID,           // Status
    NULL,                         // Data
    0,                            // Length
    DATA_FLAG_NONE                // Data Flags
  };
  return ConstructSpdyFrame(
      kSynStartHeader, ConstructPostHeaderBlock(url, content_length));
}

SpdyFrame* ConstructChunkedSpdyPostWithVersion(
    SpdyMajorVersion spdy_version,
    const char* const extra_headers[],
    int extra_header_count) {
  const char* post_headers[] = {
    ":method", "POST",
    ":path", "/",
    ":host", "www.google.com",
    ":scheme", "http",
    ":version", "HTTP/1.1"
  };
  return ConstructSpdyControlFrameWithVersion(spdy_version,
                                              extra_headers,
                                              extra_header_count,
                                              false,
                                              1,
                                              LOWEST,
                                              SYN_STREAM,
                                              CONTROL_FLAG_NONE,
                                              post_headers,
                                              arraysize(post_headers),
                                              0);
}

SpdyFrame* ConstructChunkedSpdyPost(const char* const extra_headers[],
                                    int extra_header_count) {
  return ConstructChunkedSpdyPostWithVersion(SPDY3,
                                             extra_headers,
                                             extra_header_count);
}

SpdyFrame* ConstructSpdyPostSynReplyWithVersion(
    SpdyMajorVersion spdy_version,
    const char* const extra_headers[],
    int extra_header_count) {
  static const char* const kStandardGetHeaders[] = {
    "hello", "bye",
    ":status", "200",
    "url", "/index.php",
    ":version", "HTTP/1.1"
  };
  return ConstructSpdyControlFrameWithVersion(
      spdy_version,
      extra_headers,
      extra_header_count,
      false,
      1,
      LOWEST,
      SYN_REPLY,
      CONTROL_FLAG_NONE,
      kStandardGetHeaders,
      arraysize(kStandardGetHeaders),
      0);
}

SpdyFrame* ConstructSpdyPostSynReply(const char* const extra_headers[],
                                     int extra_header_count) {
  return ConstructSpdyPostSynReplyWithVersion(SPDY3,
                                              extra_headers,
                                              extra_header_count);
}

// Constructs a single SPDY data frame with the default contents.
SpdyFrame* ConstructSpdyBodyFrame(int stream_id, bool fin) {
  BufferedSpdyFramer framer(SPDY3, false);
  return framer.CreateDataFrame(
      stream_id, kUploadData, kUploadDataSize,
      fin ? DATA_FLAG_FIN : DATA_FLAG_NONE);
}

// Constructs a single SPDY data frame with the given content.
SpdyFrame* ConstructSpdyBodyFrame(int stream_id, const char* data,
                                  uint32 len, bool fin) {
  BufferedSpdyFramer framer(SPDY3, false);
  return framer.CreateDataFrame(
      stream_id, data, len, fin ? DATA_FLAG_FIN : DATA_FLAG_NONE);
}

// Wraps |frame| in the payload of a data frame in stream |stream_id|.
SpdyFrame* ConstructWrappedSpdyFrame(const scoped_ptr<SpdyFrame>& frame,
                                     int stream_id) {
  return ConstructSpdyBodyFrame(stream_id, frame->data(),
                                frame->size(),
                                false);
}

const SpdyHeaderInfo MakeSpdyHeader(SpdyFrameType type) {
  const SpdyHeaderInfo kHeader = {
    type,                         // Kind = Syn
    1,                            // Stream ID
    0,                            // Associated stream ID
    ConvertRequestPriorityToSpdyPriority(LOWEST, 3),  // Priority
    0,                            // Credential Slot
    CONTROL_FLAG_FIN,       // Control Flags
    false,                        // Compressed
    RST_STREAM_INVALID,           // Status
    NULL,                         // Data
    0,                            // Length
    DATA_FLAG_NONE          // Data Flags
  };
  return kHeader;
}

}  // namespace test_spdy3

}  // namespace net
