// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/url_request/url_request_context_builder.h"

#include "net/test/test_server.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"

namespace net {

namespace {

// A subclass of TestServer that uses a statically-configured hostname. This is
// to work around mysterious failures in chrome_frame_net_tests. See:
// http://crbug.com/114369
class LocalHttpTestServer : public TestServer {
 public:
  explicit LocalHttpTestServer(const FilePath& document_root)
      : TestServer(TestServer::TYPE_HTTP,
                   ScopedCustomUrlRequestTestHttpHost::value(),
                   document_root) {}
  LocalHttpTestServer()
      : TestServer(TestServer::TYPE_HTTP,
                   ScopedCustomUrlRequestTestHttpHost::value(),
                   FilePath()) {}
};

class URLRequestContextBuilderTest : public PlatformTest {
 protected:
  URLRequestContextBuilderTest()
      : test_server_(
          FilePath(FILE_PATH_LITERAL("net/data/url_request_unittest"))) {}

  LocalHttpTestServer test_server_;
  URLRequestContextBuilder builder_;
};

TEST_F(URLRequestContextBuilderTest, DefaultSettings) {
  ASSERT_TRUE(test_server_.Start());

  scoped_refptr<URLRequestContext> context = builder_.Build();
  TestDelegate delegate;
  URLRequest request(test_server_.GetURL("echoheader?Foo"), &delegate);
  request.set_context(context);
  request.set_method("GET");
  request.SetExtraRequestHeaderByName("Foo", "Bar", false);
  request.Start();
  MessageLoop::current()->Run();
  EXPECT_EQ("Bar", delegate.data_received());
}

TEST_F(URLRequestContextBuilderTest, UserAgent) {
  ASSERT_TRUE(test_server_.Start());

  builder_.set_user_agent("Bar");
  scoped_refptr<URLRequestContext> context = builder_.Build();
  TestDelegate delegate;
  URLRequest request(test_server_.GetURL("echoheader?User-Agent"), &delegate);
  request.set_context(context);
  request.set_method("GET");
  request.Start();
  MessageLoop::current()->Run();
  EXPECT_EQ("Bar", delegate.data_received());
}

}  // namespace

}  // namespace net
