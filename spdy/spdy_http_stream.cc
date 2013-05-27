// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/spdy_http_stream.h"

#include <algorithm>
#include <list>

#include "base/bind.h"
#include "base/logging.h"
#include "base/message_loop.h"
#include "base/stringprintf.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_log.h"
#include "net/base/net_util.h"
#include "net/base/upload_data_stream.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_info.h"
#include "net/spdy/spdy_header_block.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/spdy/spdy_protocol.h"
#include "net/spdy/spdy_session.h"

namespace net {

SpdyHttpStream::SpdyHttpStream(SpdySession* spdy_session,
                               bool direct)
    : weak_factory_(this),
      spdy_session_(spdy_session),
      stream_closed_(false),
      closed_stream_status_(ERR_FAILED),
      closed_stream_id_(0),
      request_info_(NULL),
      has_upload_data_(false),
      response_info_(NULL),
      response_headers_received_(false),
      user_buffer_len_(0),
      raw_request_body_buf_size_(0),
      buffered_read_callback_pending_(false),
      more_read_data_pending_(false),
      direct_(direct) {}

void SpdyHttpStream::InitializeWithExistingStream(
    const base::WeakPtr<SpdyStream>& spdy_stream) {
  stream_ = spdy_stream;
  stream_->SetDelegate(this);
  response_headers_received_ = true;
}

SpdyHttpStream::~SpdyHttpStream() {
  if (stream_) {
    stream_->DetachDelegate();
    DCHECK(!stream_);
  }
}

int SpdyHttpStream::InitializeStream(const HttpRequestInfo* request_info,
                                     RequestPriority priority,
                                     const BoundNetLog& stream_net_log,
                                     const CompletionCallback& callback) {
  DCHECK(!stream_.get());
  if (spdy_session_->IsClosed())
   return ERR_CONNECTION_CLOSED;

  request_info_ = request_info;
  if (request_info_->method == "GET") {
    int error = spdy_session_->GetPushStream(request_info_->url, &stream_,
                                             stream_net_log);
    if (error != OK)
      return error;

    // |stream_| may be NULL even if OK was returned.
    if (stream_) {
      DCHECK_EQ(stream_->type(), SPDY_PUSH_STREAM);
      stream_->SetDelegate(this);
      return OK;
    }
  }

  int rv = stream_request_.StartRequest(
      SPDY_REQUEST_RESPONSE_STREAM, spdy_session_, request_info_->url,
      priority, stream_net_log,
      base::Bind(&SpdyHttpStream::OnStreamCreated,
                 weak_factory_.GetWeakPtr(), callback));

  if (rv == OK) {
    stream_ = stream_request_.ReleaseStream();
    stream_->SetDelegate(this);
  }

  return rv;
}

const HttpResponseInfo* SpdyHttpStream::GetResponseInfo() const {
  return response_info_;
}

UploadProgress SpdyHttpStream::GetUploadProgress() const {
  if (!request_info_ || !request_info_->upload_data_stream)
    return UploadProgress();

  return UploadProgress(request_info_->upload_data_stream->position(),
                        request_info_->upload_data_stream->size());
}

int SpdyHttpStream::ReadResponseHeaders(const CompletionCallback& callback) {
  CHECK(!callback.is_null());
  if (stream_closed_)
    return closed_stream_status_;

  CHECK(stream_);

  // Check if we already have the response headers. If so, return synchronously.
  if(stream_->response_received()) {
    CHECK(stream_->is_idle());
    return OK;
  }

  // Still waiting for the response, return IO_PENDING.
  CHECK(callback_.is_null());
  callback_ = callback;
  return ERR_IO_PENDING;
}

int SpdyHttpStream::ReadResponseBody(
    IOBuffer* buf, int buf_len, const CompletionCallback& callback) {
  if (stream_) {
    CHECK(stream_->is_idle());
    CHECK(!stream_->closed());
  }
  CHECK(buf);
  CHECK(buf_len);
  CHECK(!callback.is_null());

  // If we have data buffered, complete the IO immediately.
  if (!response_body_queue_.IsEmpty()) {
    return response_body_queue_.Dequeue(buf->data(), buf_len);
  } else if (stream_closed_) {
    return closed_stream_status_;
  }

  CHECK(callback_.is_null());
  CHECK(!user_buffer_);
  CHECK_EQ(0, user_buffer_len_);

  callback_ = callback;
  user_buffer_ = buf;
  user_buffer_len_ = buf_len;
  return ERR_IO_PENDING;
}

void SpdyHttpStream::Close(bool not_reusable) {
  // Note: the not_reusable flag has no meaning for SPDY streams.

  Cancel();
  DCHECK(!stream_.get());
}

HttpStream* SpdyHttpStream::RenewStreamForAuth() {
  return NULL;
}

bool SpdyHttpStream::IsResponseBodyComplete() const {
  return stream_closed_;
}

bool SpdyHttpStream::CanFindEndOfResponse() const {
  return true;
}

bool SpdyHttpStream::IsConnectionReused() const {
  return spdy_session_->IsReused();
}

void SpdyHttpStream::SetConnectionReused() {
  // SPDY doesn't need an indicator here.
}

bool SpdyHttpStream::IsConnectionReusable() const {
  // SPDY streams aren't considered reusable.
  return false;
}

bool SpdyHttpStream::GetLoadTimingInfo(LoadTimingInfo* load_timing_info) const {
  // If |stream_| has yet to be created, or does not yet have an ID, fail.
  // The reused flag can only be correctly set once a stream has an ID.  Streams
  // get their IDs once the request has been successfully sent, so this does not
  // behave that differently from other stream types.
  if (!spdy_session_ || (!stream_ && !stream_closed_))
    return false;

  SpdyStreamId stream_id =
      stream_closed_ ? closed_stream_id_ : stream_->stream_id();
  if (stream_id == 0)
    return false;

  return spdy_session_->GetLoadTimingInfo(stream_id, load_timing_info);
}

int SpdyHttpStream::SendRequest(const HttpRequestHeaders& request_headers,
                                HttpResponseInfo* response,
                                const CompletionCallback& callback) {
  if (stream_closed_) {
    if (stream_->type() == SPDY_PUSH_STREAM)
      return closed_stream_status_;

    return (closed_stream_status_ == OK) ? ERR_FAILED : closed_stream_status_;
  }

  base::Time request_time = base::Time::Now();
  CHECK(stream_.get());

  stream_->SetRequestTime(request_time);
  // This should only get called in the case of a request occurring
  // during server push that has already begun but hasn't finished,
  // so we set the response's request time to be the actual one
  if (response_info_)
    response_info_->request_time = request_time;

  CHECK(!has_upload_data_);
  has_upload_data_ = request_info_->upload_data_stream &&
      (request_info_->upload_data_stream->size() ||
       request_info_->upload_data_stream->is_chunked());
  if (has_upload_data_) {
    // Use kMaxSpdyFrameChunkSize as the buffer size, since the request
    // body data is written with this size at a time.
    raw_request_body_buf_ = new IOBufferWithSize(kMaxSpdyFrameChunkSize);
    // The request body buffer is empty at first.
    raw_request_body_buf_size_ = 0;
  }

  CHECK(!callback.is_null());
  CHECK(response);

  // SendRequest can be called in two cases.
  //
  // a) A client initiated request. In this case, |response_info_| should be
  //    NULL to start with.
  // b) A client request which matches a response that the server has already
  //    pushed.
  if (push_response_info_.get()) {
    *response = *(push_response_info_.get());
    push_response_info_.reset();
  } else {
    DCHECK_EQ(static_cast<HttpResponseInfo*>(NULL), response_info_);
  }

  response_info_ = response;

  // Put the peer's IP address and port into the response.
  IPEndPoint address;
  int result = stream_->GetPeerAddress(&address);
  if (result != OK)
    return result;
  response_info_->socket_address = HostPortPair::FromIPEndPoint(address);

  if (stream_->type() == SPDY_PUSH_STREAM) {
    // Pushed streams do not send any data, and should always be
    // idle. However, we still want to return ERR_IO_PENDING to mimic
    // non-push behavior. The callback will be called when the
    // response is received.
    result = ERR_IO_PENDING;
  } else {
    scoped_ptr<SpdyHeaderBlock> headers(new SpdyHeaderBlock);
    CreateSpdyHeadersFromHttpRequest(
        *request_info_, request_headers,
        headers.get(), stream_->GetProtocolVersion(),
        direct_);
    stream_->net_log().AddEvent(
        NetLog::TYPE_HTTP_TRANSACTION_SPDY_SEND_REQUEST_HEADERS,
        base::Bind(&SpdyHeaderBlockNetLogCallback, headers.get()));
    result =
        stream_->SendRequestHeaders(
            headers.Pass(),
            has_upload_data_ ? MORE_DATA_TO_SEND : NO_MORE_DATA_TO_SEND);
  }

  if (result == ERR_IO_PENDING) {
    CHECK(callback_.is_null());
    callback_ = callback;
  }
  return result;
}

void SpdyHttpStream::Cancel() {
  callback_.Reset();
  if (stream_) {
    stream_->Cancel();
    DCHECK(!stream_);
  }
}

void SpdyHttpStream::OnSendRequestHeadersComplete() {
  if (!callback_.is_null())
    DoCallback(OK);
}

void SpdyHttpStream::OnSendBody() {
  CHECK(request_info_ && request_info_->upload_data_stream);
  if (raw_request_body_buf_size_ > 0) {
    SendRequestBodyData();
  } else {
    // We shouldn't be called if there's no more data to read.
    CHECK(!request_info_->upload_data_stream->IsEOF());
    ReadAndSendRequestBodyData();
  }
}

void SpdyHttpStream::OnSendBodyComplete() {
  // |status| is the number of bytes written to the SPDY stream.
  CHECK(request_info_ && request_info_->upload_data_stream);
  raw_request_body_buf_size_ = 0;
}

int SpdyHttpStream::OnResponseReceived(const SpdyHeaderBlock& response,
                                       base::Time response_time,
                                       int status) {
  if (!response_info_) {
    DCHECK_EQ(stream_->type(), SPDY_PUSH_STREAM);
    push_response_info_.reset(new HttpResponseInfo);
    response_info_ = push_response_info_.get();
  }

  // If the response is already received, these headers are too late.
  if (response_headers_received_) {
    LOG(WARNING) << "SpdyHttpStream headers received after response started.";
    return OK;
  }

  // TODO(mbelshe): This is the time of all headers received, not just time
  // to first byte.
  response_info_->response_time = base::Time::Now();

  if (!SpdyHeadersToHttpResponse(response, stream_->GetProtocolVersion(),
                                 response_info_)) {
    // We might not have complete headers yet.
    return ERR_INCOMPLETE_SPDY_HEADERS;
  }

  response_headers_received_ = true;
  // Don't store the SSLInfo in the response here, HttpNetworkTransaction
  // will take care of that part.
  SSLInfo ssl_info;
  NextProto protocol_negotiated = kProtoUnknown;
  stream_->GetSSLInfo(&ssl_info,
                      &response_info_->was_npn_negotiated,
                      &protocol_negotiated);
  response_info_->npn_negotiated_protocol =
      SSLClientSocket::NextProtoToString(protocol_negotiated);
  response_info_->request_time = stream_->GetRequestTime();
  switch (spdy_session_->GetProtocolVersion()) {
    case SPDY2:
      response_info_->connection_info = HttpResponseInfo::CONNECTION_INFO_SPDY2;
      break;
    case SPDY3:
      response_info_->connection_info = HttpResponseInfo::CONNECTION_INFO_SPDY3;
      break;
    case SPDY4:
      response_info_->connection_info = HttpResponseInfo::CONNECTION_INFO_SPDY4;
      break;
    default:
      NOTREACHED();
  }
  response_info_->vary_data.Init(*request_info_, *response_info_->headers);
  // TODO(ahendrickson): This is recorded after the entire SYN_STREAM control
  // frame has been received and processed.  Move to framer?
  response_info_->response_time = response_time;

  if (!callback_.is_null())
    DoCallback(status);

  return status;
}

int SpdyHttpStream::OnDataReceived(scoped_ptr<SpdyBuffer> buffer) {
  // SpdyStream won't call us with data if the header block didn't contain a
  // valid set of headers.  So we don't expect to not have headers received
  // here.
  if (!response_headers_received_)
    return ERR_INCOMPLETE_SPDY_HEADERS;

  // Note that data may be received for a SpdyStream prior to the user calling
  // ReadResponseBody(), therefore user_buffer_ may be NULL.  This may often
  // happen for server initiated streams.
  DCHECK(stream_.get());
  DCHECK(!stream_->closed() || stream_->type() == SPDY_PUSH_STREAM);
  if (buffer) {
    response_body_queue_.Enqueue(buffer.Pass());

    if (user_buffer_) {
      // Handing small chunks of data to the caller creates measurable overhead.
      // We buffer data in short time-spans and send a single read notification.
      ScheduleBufferedReadCallback();
    }
  }
  return OK;
}

void SpdyHttpStream::OnDataSent() {
  // For HTTP streams, no data is sent from the client while in the OPEN state,
  // so it is never called.
  CHECK(false);
}

void SpdyHttpStream::OnClose(int status) {
  if (stream_) {
    stream_closed_ = true;
    closed_stream_status_ = status;
    closed_stream_id_ = stream_->stream_id();
  }
  stream_.reset();
  bool invoked_callback = false;
  if (status == net::OK) {
    // We need to complete any pending buffered read now.
    invoked_callback = DoBufferedReadCallback();
  }
  if (!invoked_callback && !callback_.is_null())
    DoCallback(status);
}

void SpdyHttpStream::OnStreamCreated(
    const CompletionCallback& callback,
    int rv) {
  if (rv == OK) {
    stream_ = stream_request_.ReleaseStream();
    stream_->SetDelegate(this);
  }
  callback.Run(rv);
}

void SpdyHttpStream::ReadAndSendRequestBodyData() {
  CHECK(request_info_ && request_info_->upload_data_stream);
  CHECK_EQ(raw_request_body_buf_size_, 0);

  // Read the data from the request body stream.
  const int rv = request_info_->upload_data_stream->Read(
      raw_request_body_buf_, raw_request_body_buf_->size(),
      base::Bind(
          &SpdyHttpStream::OnRequestBodyReadCompleted,
          weak_factory_.GetWeakPtr()));

  if (rv != ERR_IO_PENDING) {
    // ERR_IO_PENDING is the only possible error.
    CHECK_GE(rv, 0);
    OnRequestBodyReadCompleted(rv);
  }
}

void SpdyHttpStream::OnRequestBodyReadCompleted(int status) {
  CHECK_GE(status, 0);
  raw_request_body_buf_size_ = status;
  SendRequestBodyData();
}

void SpdyHttpStream::SendRequestBodyData() {
  const bool eof = request_info_->upload_data_stream->IsEOF();
  if (eof) {
    CHECK_GE(raw_request_body_buf_size_, 0);
  } else {
    CHECK_GT(raw_request_body_buf_size_, 0);
  }
  stream_->SendStreamData(raw_request_body_buf_,
                          raw_request_body_buf_size_,
                          eof ? NO_MORE_DATA_TO_SEND : MORE_DATA_TO_SEND);
}

void SpdyHttpStream::ScheduleBufferedReadCallback() {
  // If there is already a scheduled DoBufferedReadCallback, don't issue
  // another one.  Mark that we have received more data and return.
  if (buffered_read_callback_pending_) {
    more_read_data_pending_ = true;
    return;
  }

  more_read_data_pending_ = false;
  buffered_read_callback_pending_ = true;
  const base::TimeDelta kBufferTime = base::TimeDelta::FromMilliseconds(1);
  base::MessageLoop::current()->PostDelayedTask(
      FROM_HERE,
      base::Bind(base::IgnoreResult(&SpdyHttpStream::DoBufferedReadCallback),
                 weak_factory_.GetWeakPtr()),
      kBufferTime);
}

// Checks to see if we should wait for more buffered data before notifying
// the caller.  Returns true if we should wait, false otherwise.
bool SpdyHttpStream::ShouldWaitForMoreBufferedData() const {
  // If the response is complete, there is no point in waiting.
  if (stream_closed_)
    return false;

  DCHECK_GT(user_buffer_len_, 0);
  return response_body_queue_.GetTotalSize() <
      static_cast<size_t>(user_buffer_len_);
}

bool SpdyHttpStream::DoBufferedReadCallback() {
  buffered_read_callback_pending_ = false;

  // If the transaction is cancelled or errored out, we don't need to complete
  // the read.
  if (!stream_ && !stream_closed_)
    return false;

  int stream_status =
      stream_closed_ ? closed_stream_status_ : stream_->response_status();
  if (stream_status != OK)
    return false;

  // When more_read_data_pending_ is true, it means that more data has
  // arrived since we started waiting.  Wait a little longer and continue
  // to buffer.
  if (more_read_data_pending_ && ShouldWaitForMoreBufferedData()) {
    ScheduleBufferedReadCallback();
    return false;
  }

  int rv = 0;
  if (user_buffer_) {
    rv = ReadResponseBody(user_buffer_, user_buffer_len_, callback_);
    CHECK_NE(rv, ERR_IO_PENDING);
    user_buffer_ = NULL;
    user_buffer_len_ = 0;
    DoCallback(rv);
    return true;
  }
  return false;
}

void SpdyHttpStream::DoCallback(int rv) {
  CHECK_NE(rv, ERR_IO_PENDING);
  CHECK(!callback_.is_null());

  // Since Run may result in being called back, clear user_callback_ in advance.
  CompletionCallback c = callback_;
  callback_.Reset();
  c.Run(rv);
}

void SpdyHttpStream::GetSSLInfo(SSLInfo* ssl_info) {
  DCHECK(stream_);
  bool using_npn;
  NextProto protocol_negotiated = kProtoUnknown;
  stream_->GetSSLInfo(ssl_info, &using_npn, &protocol_negotiated);
}

void SpdyHttpStream::GetSSLCertRequestInfo(
    SSLCertRequestInfo* cert_request_info) {
  DCHECK(stream_);
  stream_->GetSSLCertRequestInfo(cert_request_info);
}

bool SpdyHttpStream::IsSpdyHttpStream() const {
  return true;
}

void SpdyHttpStream::Drain(HttpNetworkSession* session) {
  Close(false);
  delete this;
}

}  // namespace net
