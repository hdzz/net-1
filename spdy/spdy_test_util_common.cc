// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/spdy_test_util_common.h"

#include <cstddef>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "base/string_number_conversions.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/http/http_cache.h"
#include "net/http/http_network_session.h"
#include "net/http/http_network_transaction.h"
#include "net/http/http_server_properties_impl.h"
#include "net/socket/socket_test_util.h"
#include "net/spdy/buffered_spdy_framer.h"
#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/spdy/spdy_session.h"
#include "net/spdy/spdy_stream.h"

namespace net {

namespace {

bool next_proto_is_spdy(NextProto next_proto) {
  return next_proto >= kProtoSPDYMinimumVersion &&
         next_proto <= kProtoSPDYMaximumVersion;
}

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

}  // namespace

// Chop a frame into an array of MockWrites.
// |data| is the frame to chop.
// |length| is the length of the frame to chop.
// |num_chunks| is the number of chunks to create.
MockWrite* ChopWriteFrame(const char* data, int length, int num_chunks) {
  MockWrite* chunks = new MockWrite[num_chunks];
  int chunk_size = length / num_chunks;
  for (int index = 0; index < num_chunks; index++) {
    const char* ptr = data + (index * chunk_size);
    if (index == num_chunks - 1)
      chunk_size += length % chunk_size;  // The last chunk takes the remainder.
    chunks[index] = MockWrite(ASYNC, ptr, chunk_size);
  }
  return chunks;
}

// Chop a SpdyFrame into an array of MockWrites.
// |frame| is the frame to chop.
// |num_chunks| is the number of chunks to create.
MockWrite* ChopWriteFrame(const SpdyFrame& frame, int num_chunks) {
  return ChopWriteFrame(frame.data(), frame.size(), num_chunks);
}

// Chop a frame into an array of MockReads.
// |data| is the frame to chop.
// |length| is the length of the frame to chop.
// |num_chunks| is the number of chunks to create.
MockRead* ChopReadFrame(const char* data, int length, int num_chunks) {
  MockRead* chunks = new MockRead[num_chunks];
  int chunk_size = length / num_chunks;
  for (int index = 0; index < num_chunks; index++) {
    const char* ptr = data + (index * chunk_size);
    if (index == num_chunks - 1)
      chunk_size += length % chunk_size;  // The last chunk takes the remainder.
    chunks[index] = MockRead(ASYNC, ptr, chunk_size);
  }
  return chunks;
}

// Chop a SpdyFrame into an array of MockReads.
// |frame| is the frame to chop.
// |num_chunks| is the number of chunks to create.
MockRead* ChopReadFrame(const SpdyFrame& frame, int num_chunks) {
  return ChopReadFrame(frame.data(), frame.size(), num_chunks);
}

// Adds headers and values to a map.
// |extra_headers| is an array of { name, value } pairs, arranged as strings
// where the even entries are the header names, and the odd entries are the
// header values.
// |headers| gets filled in from |extra_headers|.
void AppendToHeaderBlock(const char* const extra_headers[],
                         int extra_header_count,
                         SpdyHeaderBlock* headers) {
  std::string this_header;
  std::string this_value;

  if (!extra_header_count)
    return;

  // Sanity check: Non-NULL header list.
  DCHECK(NULL != extra_headers) << "NULL header value pair list";
  // Sanity check: Non-NULL header map.
  DCHECK(NULL != headers) << "NULL header map";
  // Copy in the headers.
  for (int i = 0; i < extra_header_count; i++) {
    // Sanity check: Non-empty header.
    DCHECK_NE('\0', *extra_headers[i * 2]) << "Empty header value pair";
    this_header = extra_headers[i * 2];
    std::string::size_type header_len = this_header.length();
    if (!header_len)
      continue;
    this_value = extra_headers[1 + (i * 2)];
    std::string new_value;
    if (headers->find(this_header) != headers->end()) {
      // More than one entry in the header.
      // Don't add the header again, just the append to the value,
      // separated by a NULL character.

      // Adjust the value.
      new_value = (*headers)[this_header];
      // Put in a NULL separator.
      new_value.append(1, '\0');
      // Append the new value.
      new_value += this_value;
    } else {
      // Not a duplicate, just write the value.
      new_value = this_value;
    }
    (*headers)[this_header] = new_value;
  }
}

// Writes |val| to a location of size |len|, in big-endian format.
// in the buffer pointed to by |buffer_handle|.
// Updates the |*buffer_handle| pointer by |len|
// Returns the number of bytes written
int AppendToBuffer(int val,
                   int len,
                   unsigned char** buffer_handle,
                   int* buffer_len_remaining) {
  if (len <= 0)
    return 0;
  DCHECK((size_t) len <= sizeof(len)) << "Data length too long for data type";
  DCHECK(NULL != buffer_handle) << "NULL buffer handle";
  DCHECK(NULL != *buffer_handle) << "NULL pointer";
  DCHECK(NULL != buffer_len_remaining)
      << "NULL buffer remainder length pointer";
  DCHECK_GE(*buffer_len_remaining, len) << "Insufficient buffer size";
  for (int i = 0; i < len; i++) {
    int shift = (8 * (len - (i + 1)));
    unsigned char val_chunk = (val >> shift) & 0x0FF;
    *(*buffer_handle)++ = val_chunk;
    *buffer_len_remaining += 1;
  }
  return len;
}

// Create a MockWrite from the given SpdyFrame.
MockWrite CreateMockWrite(const SpdyFrame& req) {
  return MockWrite(ASYNC, req.data(), req.size());
}

// Create a MockWrite from the given SpdyFrame and sequence number.
MockWrite CreateMockWrite(const SpdyFrame& req, int seq) {
  return CreateMockWrite(req, seq, ASYNC);
}

// Create a MockWrite from the given SpdyFrame and sequence number.
MockWrite CreateMockWrite(const SpdyFrame& req, int seq, IoMode mode) {
  return MockWrite(mode, req.data(), req.size(), seq);
}

// Create a MockRead from the given SpdyFrame.
MockRead CreateMockRead(const SpdyFrame& resp) {
  return MockRead(ASYNC, resp.data(), resp.size());
}

// Create a MockRead from the given SpdyFrame and sequence number.
MockRead CreateMockRead(const SpdyFrame& resp, int seq) {
  return CreateMockRead(resp, seq, ASYNC);
}

// Create a MockRead from the given SpdyFrame and sequence number.
MockRead CreateMockRead(const SpdyFrame& resp, int seq, IoMode mode) {
  return MockRead(mode, resp.data(), resp.size(), seq);
}

// Combines the given SpdyFrames into the given char array and returns
// the total length.
int CombineFrames(const SpdyFrame** frames, int num_frames,
                  char* buff, int buff_len) {
  int total_len = 0;
  for (int i = 0; i < num_frames; ++i) {
    total_len += frames[i]->size();
  }
  DCHECK_LE(total_len, buff_len);
  char* ptr = buff;
  for (int i = 0; i < num_frames; ++i) {
    int len = frames[i]->size();
    memcpy(ptr, frames[i]->data(), len);
    ptr += len;
  }
  return total_len;
}

namespace {

class PriorityGetter : public BufferedSpdyFramerVisitorInterface {
 public:
  PriorityGetter() : priority_(0) {}
  virtual ~PriorityGetter() {}

  SpdyPriority priority() const {
    return priority_;
  }

  virtual void OnError(SpdyFramer::SpdyError error_code) OVERRIDE {}
  virtual void OnStreamError(SpdyStreamId stream_id,
                             const std::string& description) OVERRIDE {}
  virtual void OnSynStream(SpdyStreamId stream_id,
                           SpdyStreamId associated_stream_id,
                           SpdyPriority priority,
                           uint8 credential_slot,
                           bool fin,
                           bool unidirectional,
                           const SpdyHeaderBlock& headers) OVERRIDE {
    priority_ = priority;
  }
  virtual void OnSynReply(SpdyStreamId stream_id,
                          bool fin,
                          const SpdyHeaderBlock& headers) OVERRIDE {}
  virtual void OnHeaders(SpdyStreamId stream_id,
                         bool fin,
                         const SpdyHeaderBlock& headers) OVERRIDE {}
  virtual void OnStreamFrameData(SpdyStreamId stream_id,
                                 const char* data,
                                 size_t len,
                                 bool fin) OVERRIDE {}
  virtual void OnSettings(bool clear_persisted) OVERRIDE {}
  virtual void OnSetting(
      SpdySettingsIds id, uint8 flags, uint32 value) OVERRIDE {}
  virtual void OnPing(uint32 unique_id) OVERRIDE {}
  virtual void OnRstStream(SpdyStreamId stream_id,
                           SpdyRstStreamStatus status) OVERRIDE {}
  virtual void OnGoAway(SpdyStreamId last_accepted_stream_id,
                        SpdyGoAwayStatus status) OVERRIDE {}
  virtual void OnWindowUpdate(SpdyStreamId stream_id,
                              uint32 delta_window_size) OVERRIDE {}
  virtual void OnSynStreamCompressed(
      size_t uncompressed_size,
      size_t compressed_size) OVERRIDE {}

 private:
  SpdyPriority priority_;
};

}  // namespace

bool GetSpdyPriority(int version,
                     const SpdyFrame& frame,
                     SpdyPriority* priority) {
  BufferedSpdyFramer framer(version, false);
  PriorityGetter priority_getter;
  framer.set_visitor(&priority_getter);
  size_t frame_size = frame.size();
  if (framer.ProcessInput(frame.data(), frame_size) != frame_size) {
    return false;
  }
  *priority = priority_getter.priority();
  return true;
}

base::WeakPtr<SpdyStream> CreateStreamSynchronously(
    const scoped_refptr<SpdySession>& session,
    const GURL& url,
    RequestPriority priority,
    const BoundNetLog& net_log) {
  SpdyStreamRequest stream_request;
  int rv = stream_request.StartRequest(session, url, priority, net_log,
                                       CompletionCallback());
  return
      (rv == OK) ? stream_request.ReleaseStream() : base::WeakPtr<SpdyStream>();
}

StreamReleaserCallback::StreamReleaserCallback() {}

StreamReleaserCallback::~StreamReleaserCallback() {}

CompletionCallback StreamReleaserCallback::MakeCallback(
    SpdyStreamRequest* request) {
  return base::Bind(&StreamReleaserCallback::OnComplete,
                    base::Unretained(this),
                    request);
}

void StreamReleaserCallback::OnComplete(
    SpdyStreamRequest* request, int result) {
  request->ReleaseStream()->Cancel();
  SetResult(result);
}

MockECSignatureCreator::MockECSignatureCreator(crypto::ECPrivateKey* key)
    : key_(key) {
}

bool MockECSignatureCreator::Sign(const uint8* data,
                                  int data_len,
                                  std::vector<uint8>* signature) {
  std::vector<uint8> private_key_value;
  key_->ExportValue(&private_key_value);
  std::string head = "fakesignature";
  std::string tail = "/fakesignature";

  signature->clear();
  signature->insert(signature->end(), head.begin(), head.end());
  signature->insert(signature->end(), private_key_value.begin(),
                    private_key_value.end());
  signature->insert(signature->end(), '-');
  signature->insert(signature->end(), data, data + data_len);
  signature->insert(signature->end(), tail.begin(), tail.end());
  return true;
}

bool MockECSignatureCreator::DecodeSignature(
    const std::vector<uint8>& signature,
    std::vector<uint8>* out_raw_sig) {
  *out_raw_sig = signature;
  return true;
}

MockECSignatureCreatorFactory::MockECSignatureCreatorFactory() {
  crypto::ECSignatureCreator::SetFactoryForTesting(this);
}

MockECSignatureCreatorFactory::~MockECSignatureCreatorFactory() {
  crypto::ECSignatureCreator::SetFactoryForTesting(NULL);
}

crypto::ECSignatureCreator* MockECSignatureCreatorFactory::Create(
    crypto::ECPrivateKey* key) {
  return new MockECSignatureCreator(key);
}

SpdySessionDependencies::SpdySessionDependencies(NextProto protocol)
    : host_resolver(new MockCachingHostResolver),
      cert_verifier(new MockCertVerifier),
      proxy_service(ProxyService::CreateDirect()),
      ssl_config_service(new SSLConfigServiceDefaults),
      socket_factory(new MockClientSocketFactory),
      deterministic_socket_factory(new DeterministicMockClientSocketFactory),
      http_auth_handler_factory(
          HttpAuthHandlerFactory::CreateDefault(host_resolver.get())),
      enable_ip_pooling(true),
      enable_compression(false),
      enable_ping(false),
      enable_user_alternate_protocol_ports(false),
      protocol(protocol),
      stream_initial_recv_window_size(kSpdyStreamInitialWindowSize),
      time_func(&base::TimeTicks::Now),
      net_log(NULL) {
  DCHECK(next_proto_is_spdy(protocol)) << "Invalid protocol: " << protocol;

  // Note: The CancelledTransaction test does cleanup by running all
  // tasks in the message loop (RunAllPending).  Unfortunately, that
  // doesn't clean up tasks on the host resolver thread; and
  // TCPConnectJob is currently not cancellable.  Using synchronous
  // lookups allows the test to shutdown cleanly.  Until we have
  // cancellable TCPConnectJobs, use synchronous lookups.
  host_resolver->set_synchronous_mode(true);
}

SpdySessionDependencies::SpdySessionDependencies(
    NextProto protocol, ProxyService* proxy_service)
    : host_resolver(new MockHostResolver),
      cert_verifier(new MockCertVerifier),
      proxy_service(proxy_service),
      ssl_config_service(new SSLConfigServiceDefaults),
      socket_factory(new MockClientSocketFactory),
      deterministic_socket_factory(new DeterministicMockClientSocketFactory),
      http_auth_handler_factory(
          HttpAuthHandlerFactory::CreateDefault(host_resolver.get())),
      enable_ip_pooling(true),
      enable_compression(false),
      enable_ping(false),
      enable_user_alternate_protocol_ports(false),
      protocol(protocol),
      stream_initial_recv_window_size(kSpdyStreamInitialWindowSize),
      time_func(&base::TimeTicks::Now),
      net_log(NULL) {
  DCHECK(next_proto_is_spdy(protocol)) << "Invalid protocol: " << protocol;
}

SpdySessionDependencies::~SpdySessionDependencies() {}

// static
HttpNetworkSession* SpdySessionDependencies::SpdyCreateSession(
    SpdySessionDependencies* session_deps) {
  net::HttpNetworkSession::Params params = CreateSessionParams(session_deps);
  params.client_socket_factory = session_deps->socket_factory.get();
  HttpNetworkSession* http_session = new HttpNetworkSession(params);
  SpdySessionPoolPeer pool_peer(http_session->spdy_session_pool());
  pool_peer.EnableSendingInitialSettings(false);
  return http_session;
}

// static
HttpNetworkSession* SpdySessionDependencies::SpdyCreateSessionDeterministic(
    SpdySessionDependencies* session_deps) {
  net::HttpNetworkSession::Params params = CreateSessionParams(session_deps);
  params.client_socket_factory =
      session_deps->deterministic_socket_factory.get();
  HttpNetworkSession* http_session = new HttpNetworkSession(params);
  SpdySessionPoolPeer pool_peer(http_session->spdy_session_pool());
  pool_peer.EnableSendingInitialSettings(false);
  return http_session;
}

// static
net::HttpNetworkSession::Params SpdySessionDependencies::CreateSessionParams(
    SpdySessionDependencies* session_deps) {
  DCHECK(next_proto_is_spdy(session_deps->protocol)) <<
      "Invalid protocol: " << session_deps->protocol;

  net::HttpNetworkSession::Params params;
  params.host_resolver = session_deps->host_resolver.get();
  params.cert_verifier = session_deps->cert_verifier.get();
  params.proxy_service = session_deps->proxy_service.get();
  params.ssl_config_service = session_deps->ssl_config_service;
  params.http_auth_handler_factory =
      session_deps->http_auth_handler_factory.get();
  params.http_server_properties = &session_deps->http_server_properties;
  params.enable_spdy_compression = session_deps->enable_compression;
  params.enable_spdy_ping_based_connection_checking = session_deps->enable_ping;
  params.enable_user_alternate_protocol_ports =
      session_deps->enable_user_alternate_protocol_ports;
  params.spdy_default_protocol = session_deps->protocol;
  params.spdy_stream_initial_recv_window_size =
      session_deps->stream_initial_recv_window_size;
  params.time_func = session_deps->time_func;
  params.trusted_spdy_proxy = session_deps->trusted_spdy_proxy;
  params.net_log = session_deps->net_log;
  return params;
}

SpdyURLRequestContext::SpdyURLRequestContext(NextProto protocol)
    : storage_(this) {
  DCHECK(next_proto_is_spdy(protocol)) << "Invalid protocol: " << protocol;

  storage_.set_host_resolver(scoped_ptr<HostResolver>(new MockHostResolver));
  storage_.set_cert_verifier(new MockCertVerifier);
  storage_.set_proxy_service(ProxyService::CreateDirect());
  storage_.set_ssl_config_service(new SSLConfigServiceDefaults);
  storage_.set_http_auth_handler_factory(HttpAuthHandlerFactory::CreateDefault(
      host_resolver()));
  storage_.set_http_server_properties(new HttpServerPropertiesImpl);
  net::HttpNetworkSession::Params params;
  params.client_socket_factory = &socket_factory_;
  params.host_resolver = host_resolver();
  params.cert_verifier = cert_verifier();
  params.proxy_service = proxy_service();
  params.ssl_config_service = ssl_config_service();
  params.http_auth_handler_factory = http_auth_handler_factory();
  params.network_delegate = network_delegate();
  params.enable_spdy_compression = false;
  params.enable_spdy_ping_based_connection_checking = false;
  params.spdy_default_protocol = protocol;
  params.http_server_properties = http_server_properties();
  scoped_refptr<HttpNetworkSession> network_session(
      new HttpNetworkSession(params));
  SpdySessionPoolPeer pool_peer(network_session->spdy_session_pool());
  pool_peer.EnableSendingInitialSettings(false);
  storage_.set_http_transaction_factory(new HttpCache(
      network_session,
      HttpCache::DefaultBackend::InMemory(0)));
}

SpdyURLRequestContext::~SpdyURLRequestContext() {
}

SpdySessionPoolPeer::SpdySessionPoolPeer(SpdySessionPool* pool) : pool_(pool) {
}

void SpdySessionPoolPeer::AddAlias(
    const IPEndPoint& address,
    const HostPortProxyPair& pair) {
  pool_->AddAlias(address, pair);
}

void SpdySessionPoolPeer::RemoveAliases(const HostPortProxyPair& pair) {
  pool_->RemoveAliases(pair);
}

void SpdySessionPoolPeer::RemoveSpdySession(
    const scoped_refptr<SpdySession>& session) {
  pool_->Remove(session);
}

void SpdySessionPoolPeer::DisableDomainAuthenticationVerification() {
  pool_->verify_domain_authentication_ = false;
}

void SpdySessionPoolPeer::EnableSendingInitialSettings(bool enabled) {
  pool_->enable_sending_initial_settings_ = enabled;
}

NextProto NextProtoFromSpdyVersion(int spdy_version) {
  switch (spdy_version) {
  case kSpdyVersion2:
    return kProtoSPDY2;
  case kSpdyVersion3:
    return kProtoSPDY3;
  case kSpdyVersion4:
    return kProtoSPDY4a2;
  default:
    NOTREACHED();
    return kProtoUnknown;
  }
}

int SpdyVersionFromNextProto(NextProto next_proto) {
  switch (next_proto) {
  case kProtoSPDY2:
  case kProtoSPDY21:
    return kSpdyVersion2;
  case kProtoSPDY3:
  case kProtoSPDY31:
    return kSpdyVersion3;
  case kProtoSPDY4a2:
    return kSpdyVersion4;
  default:
    NOTREACHED();
    return 0;
  }
}

SpdyTestUtil::SpdyTestUtil(NextProto protocol)
    : protocol_(protocol),
      spdy_version_(SpdyVersionFromNextProto(protocol)) {
  DCHECK(next_proto_is_spdy(protocol)) << "Invalid protocol: " << protocol;
}

scoped_ptr<SpdyHeaderBlock> SpdyTestUtil::ConstructGetHeaderBlock(
    base::StringPiece url) const {
  std::string scheme, host, path;
  ParseUrl(url.data(), &scheme, &host, &path);
  const char* const headers[] = {
    is_spdy2() ? "method"  : ":method",  "GET",
    is_spdy2() ? "url"     : ":path",    path.c_str(),
    is_spdy2() ? "host"    : ":host",    host.c_str(),
    is_spdy2() ? "scheme"  : ":scheme",  scheme.c_str(),
    is_spdy2() ? "version" : ":version", "HTTP/1.1"
  };
  scoped_ptr<SpdyHeaderBlock> header_block(new SpdyHeaderBlock());
  AppendToHeaderBlock(headers, arraysize(headers) / 2, header_block.get());
  return header_block.Pass();
}

scoped_ptr<SpdyHeaderBlock> SpdyTestUtil::ConstructPostHeaderBlock(
    base::StringPiece url,
    int64 content_length) const {
  std::string scheme, host, path;
  ParseUrl(url.data(), &scheme, &host, &path);
  std::string length_str = base::Int64ToString(content_length);
  const char* const headers[] = {
    is_spdy2() ? "method"  : ":method",  "POST",
    is_spdy2() ? "url"     : ":path",    path.c_str(),
    is_spdy2() ? "host"    : ":host",    host.c_str(),
    is_spdy2() ? "scheme"  : ":scheme",  scheme.c_str(),
    is_spdy2() ? "version" : ":version", "HTTP/1.1",
    "content-length",               length_str.c_str()
  };
  scoped_ptr<SpdyHeaderBlock> header_block(new SpdyHeaderBlock());
  AppendToHeaderBlock(headers, arraysize(headers) / 2, header_block.get());
  return header_block.Pass();
}

SpdyFrame* SpdyTestUtil::ConstructSpdyFrame(
    const SpdyHeaderInfo& header_info,
    scoped_ptr<SpdyHeaderBlock> headers) const {
  BufferedSpdyFramer framer(spdy_version_, header_info.compressed);
  SpdyFrame* frame = NULL;
  switch (header_info.kind) {
    case DATA:
      frame = framer.CreateDataFrame(header_info.id, header_info.data,
                                     header_info.data_length,
                                     header_info.data_flags);
      break;
    case SYN_STREAM:
      {
        size_t credential_slot = is_spdy2() ? 0 : header_info.credential_slot;
        frame = framer.CreateSynStream(header_info.id, header_info.assoc_id,
                                       header_info.priority,
                                       credential_slot,
                                       header_info.control_flags,
                                       header_info.compressed, headers.get());
      }
      break;
    case SYN_REPLY:
      frame = framer.CreateSynReply(header_info.id, header_info.control_flags,
                                    header_info.compressed, headers.get());
      break;
    case RST_STREAM:
      frame = framer.CreateRstStream(header_info.id, header_info.status);
      break;
    case HEADERS:
      frame = framer.CreateHeaders(header_info.id, header_info.control_flags,
                                   header_info.compressed, headers.get());
      break;
    default:
      ADD_FAILURE();
      break;
  }
  return frame;
}

SpdyFrame* SpdyTestUtil::ConstructSpdyFrame(const SpdyHeaderInfo& header_info,
                                            const char* const extra_headers[],
                                            int extra_header_count,
                                            const char* const tail_headers[],
                                            int tail_header_count) const {
  scoped_ptr<SpdyHeaderBlock> headers(new SpdyHeaderBlock());
  AppendToHeaderBlock(extra_headers, extra_header_count, headers.get());
  if (tail_headers && tail_header_count)
    AppendToHeaderBlock(tail_headers, tail_header_count, headers.get());
  return ConstructSpdyFrame(header_info, headers.Pass());
}

SpdyFrame* SpdyTestUtil::ConstructSpdyControlFrame(
    const char* const extra_headers[],
    int extra_header_count,
    bool compressed,
    SpdyStreamId stream_id,
    RequestPriority request_priority,
    SpdyFrameType type,
    SpdyControlFlags flags,
    const char* const* tail_headers,
    int tail_header_size,
    SpdyStreamId associated_stream_id) const {
  EXPECT_GE(type, FIRST_CONTROL_TYPE);
  EXPECT_LE(type, LAST_CONTROL_TYPE);
  const SpdyHeaderInfo header_info = {
    type,
    stream_id,
    associated_stream_id,
    ConvertRequestPriorityToSpdyPriority(request_priority, spdy_version_),
    0,  // credential slot
    flags,
    compressed,
    RST_STREAM_INVALID,  // status
    NULL,  // data
    0,  // length
    DATA_FLAG_NONE
  };
  scoped_ptr<SpdyHeaderBlock> headers(new SpdyHeaderBlock());
  AppendToHeaderBlock(extra_headers, extra_header_count, headers.get());
  if (tail_headers && tail_header_size)
    AppendToHeaderBlock(tail_headers, tail_header_size / 2, headers.get());
  return ConstructSpdyFrame(header_info, headers.Pass());
}

SpdyFrame* SpdyTestUtil::ConstructSpdySettings(
    const SettingsMap& settings) const {
  return CreateFramer()->CreateSettings(settings);
}

SpdyFrame* SpdyTestUtil::ConstructSpdyCredential(
    const SpdyCredential& credential) const {
  return CreateFramer()->CreateCredentialFrame(credential);
}

SpdyFrame* SpdyTestUtil::ConstructSpdyPing(uint32 ping_id) const {
  return CreateFramer()->CreatePingFrame(ping_id);
}

SpdyFrame* SpdyTestUtil::ConstructSpdyGoAway() const {
  return ConstructSpdyGoAway(0);
}

SpdyFrame* SpdyTestUtil::ConstructSpdyGoAway(
    SpdyStreamId last_good_stream_id) const {
  return CreateFramer()->CreateGoAway(last_good_stream_id, GOAWAY_OK);
}

SpdyFrame* SpdyTestUtil::ConstructSpdyWindowUpdate(
    const SpdyStreamId stream_id, uint32 delta_window_size) const {
  return CreateFramer()->CreateWindowUpdate(stream_id, delta_window_size);
}

SpdyFrame* SpdyTestUtil::ConstructSpdyRstStream(
    SpdyStreamId stream_id,
    SpdyRstStreamStatus status) const {
  return CreateFramer()->CreateRstStream(stream_id, status);
}

scoped_ptr<SpdyFramer> SpdyTestUtil::CreateFramer() const {
  return scoped_ptr<SpdyFramer>(new SpdyFramer(spdy_version_));
}

}  // namespace net
