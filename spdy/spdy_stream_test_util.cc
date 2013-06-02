// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/spdy_stream_test_util.h"

#include <cstddef>

#include "base/stl_util.h"
#include "net/base/completion_callback.h"
#include "net/spdy/spdy_stream.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace test {

ClosingDelegate::ClosingDelegate(
    const base::WeakPtr<SpdyStream>& stream) : stream_(stream) {}

ClosingDelegate::~ClosingDelegate() {}

void ClosingDelegate::OnRequestHeadersSent() {}

int ClosingDelegate::OnResponseHeadersReceived(const SpdyHeaderBlock& response,
                                               base::Time response_time,
                                               int status) {
  return OK;
}

int ClosingDelegate::OnDataReceived(scoped_ptr<SpdyBuffer> buffer) {
  return OK;
}

void ClosingDelegate::OnDataSent() {}

void ClosingDelegate::OnClose(int status) {
  DCHECK(stream_.get());
  stream_->Close();
  DCHECK(!stream_.get());
}

StreamDelegateBase::StreamDelegateBase(
    const base::WeakPtr<SpdyStream>& stream)
    : stream_(stream),
      stream_id_(0),
      send_headers_completed_(false) {
}

StreamDelegateBase::~StreamDelegateBase() {
}

void StreamDelegateBase::OnRequestHeadersSent() {
  stream_id_ = stream_->stream_id();
  EXPECT_NE(stream_id_, 0u);
  send_headers_completed_ = true;
}

int StreamDelegateBase::OnResponseHeadersReceived(
    const SpdyHeaderBlock& response,
    base::Time response_time,
    int status) {
  EXPECT_TRUE(send_headers_completed_);
  response_ = response;
  return status;
}

int StreamDelegateBase::OnDataReceived(scoped_ptr<SpdyBuffer> buffer) {
  if (buffer)
    received_data_queue_.Enqueue(buffer.Pass());
  return OK;
}

void StreamDelegateBase::OnDataSent() {}

void StreamDelegateBase::OnClose(int status) {
  if (!stream_.get())
    return;
  stream_id_ = stream_->stream_id();
  stream_.reset();
  callback_.callback().Run(status);
}

int StreamDelegateBase::WaitForClose() {
  int result = callback_.WaitForResult();
  EXPECT_TRUE(!stream_.get());
  return result;
}

std::string StreamDelegateBase::TakeReceivedData() {
  size_t len = received_data_queue_.GetTotalSize();
  std::string received_data(len, '\0');
  if (len > 0) {
    EXPECT_EQ(
        len,
        received_data_queue_.Dequeue(string_as_array(&received_data), len));
  }
  return received_data;
}

std::string StreamDelegateBase::GetResponseHeaderValue(
    const std::string& name) const {
  SpdyHeaderBlock::const_iterator it = response_.find(name);
  return (it == response_.end()) ? std::string() : it->second;
}

StreamDelegateDoNothing::StreamDelegateDoNothing(
    const base::WeakPtr<SpdyStream>& stream)
    : StreamDelegateBase(stream) {}

StreamDelegateDoNothing::~StreamDelegateDoNothing() {
}

StreamDelegateSendImmediate::StreamDelegateSendImmediate(
    const base::WeakPtr<SpdyStream>& stream,
    base::StringPiece data)
    : StreamDelegateBase(stream),
      data_(data) {}

StreamDelegateSendImmediate::~StreamDelegateSendImmediate() {
}

int StreamDelegateSendImmediate::OnResponseHeadersReceived(
    const SpdyHeaderBlock& response,
    base::Time response_time,
    int status) {
  status =
      StreamDelegateBase::OnResponseHeadersReceived(
          response, response_time, status);
  if (data_.data()) {
    scoped_refptr<StringIOBuffer> buf(new StringIOBuffer(data_.as_string()));
    stream()->SendData(buf.get(), buf->size(), MORE_DATA_TO_SEND);
  }
  return status;
}

StreamDelegateWithBody::StreamDelegateWithBody(
    const base::WeakPtr<SpdyStream>& stream,
    base::StringPiece data)
    : StreamDelegateBase(stream),
      buf_(new StringIOBuffer(data.as_string())) {}

StreamDelegateWithBody::~StreamDelegateWithBody() {
}

void StreamDelegateWithBody::OnRequestHeadersSent() {
  StreamDelegateBase::OnRequestHeadersSent();
  stream()->SendData(buf_.get(), buf_->size(), NO_MORE_DATA_TO_SEND);
}

} // namespace test

} // namespace net
