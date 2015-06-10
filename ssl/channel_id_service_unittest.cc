// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/ssl/channel_id_service.h"

#include <string>
#include <vector>

#include "base/bind.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "base/message_loop/message_loop_proxy.h"
#include "base/strings/string_number_conversions.h"
#include "base/task_runner.h"
#include "crypto/ec_private_key.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "net/cert/asn1_util.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/default_channel_id_store.h"
#include "net/test/channel_id_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace {

void FailTest(int /* result */) {
  FAIL();
}

// Simple task runner that refuses to actually post any tasks. This simulates
// a TaskRunner that has been shutdown, by returning false for any attempt to
// add new tasks.
class FailingTaskRunner : public base::TaskRunner {
 public:
  FailingTaskRunner() {}

  bool PostDelayedTask(const tracked_objects::Location& from_here,
                       const base::Closure& task,
                       base::TimeDelta delay) override {
    return false;
  }

  bool RunsTasksOnCurrentThread() const override { return true; }

 protected:
  ~FailingTaskRunner() override {}

 private:
  DISALLOW_COPY_AND_ASSIGN(FailingTaskRunner);
};

class MockChannelIDStoreWithAsyncGet
    : public DefaultChannelIDStore {
 public:
  MockChannelIDStoreWithAsyncGet()
      : DefaultChannelIDStore(NULL), channel_id_count_(0) {}

  int GetChannelID(const std::string& server_identifier,
                   scoped_ptr<crypto::ECPrivateKey>* key_result,
                   const GetChannelIDCallback& callback) override;

  void SetChannelID(scoped_ptr<ChannelID> channel_id) override {
    channel_id_count_ = 1;
  }

  int GetChannelIDCount() override { return channel_id_count_; }

  void CallGetChannelIDCallbackWithResult(int err, crypto::ECPrivateKey* key);

 private:
  GetChannelIDCallback callback_;
  std::string server_identifier_;
  int channel_id_count_;
};

int MockChannelIDStoreWithAsyncGet::GetChannelID(
    const std::string& server_identifier,
    scoped_ptr<crypto::ECPrivateKey>* key_result,
    const GetChannelIDCallback& callback) {
  server_identifier_ = server_identifier;
  callback_ = callback;
  // Reset the cert count, it'll get incremented in either SetChannelID or
  // CallGetChannelIDCallbackWithResult.
  channel_id_count_ = 0;
  // Do nothing else: the results to be provided will be specified through
  // CallGetChannelIDCallbackWithResult.
  return ERR_IO_PENDING;
}

void MockChannelIDStoreWithAsyncGet::CallGetChannelIDCallbackWithResult(
    int err,
    crypto::ECPrivateKey* key) {
  if (err == OK)
    channel_id_count_ = 1;
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback_, err, server_identifier_,
                 base::Passed(make_scoped_ptr(key ? key->Copy() : nullptr))));
}

class ChannelIDServiceTest : public testing::Test {
 public:
  ChannelIDServiceTest()
      : service_(new ChannelIDService(
            new DefaultChannelIDStore(NULL),
            base::MessageLoopProxy::current())) {
  }

 protected:
  scoped_ptr<ChannelIDService> service_;
};

TEST_F(ChannelIDServiceTest, GetDomainForHost) {
  EXPECT_EQ("google.com",
            ChannelIDService::GetDomainForHost("google.com"));
  EXPECT_EQ("google.com",
            ChannelIDService::GetDomainForHost("www.google.com"));
  EXPECT_EQ("foo.appspot.com",
            ChannelIDService::GetDomainForHost("foo.appspot.com"));
  EXPECT_EQ("bar.appspot.com",
            ChannelIDService::GetDomainForHost("foo.bar.appspot.com"));
  EXPECT_EQ("appspot.com",
            ChannelIDService::GetDomainForHost("appspot.com"));
  EXPECT_EQ("google.com",
            ChannelIDService::GetDomainForHost("www.mail.google.com"));
  EXPECT_EQ("goto",
            ChannelIDService::GetDomainForHost("goto"));
  EXPECT_EQ("127.0.0.1",
            ChannelIDService::GetDomainForHost("127.0.0.1"));
}

TEST_F(ChannelIDServiceTest, GetCacheMiss) {
  std::string host("encrypted.google.com");

  int error;
  TestCompletionCallback callback;
  ChannelIDService::Request request;

  // Synchronous completion, because the store is initialized.
  scoped_ptr<crypto::ECPrivateKey> key;
  EXPECT_EQ(0, service_->channel_id_count());
  error = service_->GetChannelID(host, &key, callback.callback(), &request);
  EXPECT_EQ(ERR_FILE_NOT_FOUND, error);
  EXPECT_FALSE(request.is_active());
  EXPECT_EQ(0, service_->channel_id_count());
  EXPECT_FALSE(key);
}

TEST_F(ChannelIDServiceTest, CacheHit) {
  std::string host("encrypted.google.com");

  int error;
  TestCompletionCallback callback;
  ChannelIDService::Request request;

  // Asynchronous completion.
  scoped_ptr<crypto::ECPrivateKey> key1;
  EXPECT_EQ(0, service_->channel_id_count());
  error = service_->GetOrCreateChannelID(host, &key1, callback.callback(),
                                         &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());
  error = callback.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_EQ(1, service_->channel_id_count());
  EXPECT_TRUE(key1);
  EXPECT_FALSE(request.is_active());

  // Synchronous completion.
  scoped_ptr<crypto::ECPrivateKey> key2;
  error = service_->GetOrCreateChannelID(host, &key2, callback.callback(),
                                         &request);
  EXPECT_FALSE(request.is_active());
  EXPECT_EQ(OK, error);
  EXPECT_EQ(1, service_->channel_id_count());
  EXPECT_TRUE(KeysEqual(key1.get(), key2.get()));

  // Synchronous get.
  scoped_ptr<crypto::ECPrivateKey> key3;
  error = service_->GetChannelID(host, &key3, callback.callback(), &request);
  EXPECT_FALSE(request.is_active());
  EXPECT_EQ(OK, error);
  EXPECT_EQ(1, service_->channel_id_count());
  EXPECT_TRUE(KeysEqual(key1.get(), key3.get()));

  EXPECT_EQ(3u, service_->requests());
  EXPECT_EQ(2u, service_->key_store_hits());
  EXPECT_EQ(0u, service_->inflight_joins());
}

TEST_F(ChannelIDServiceTest, StoreChannelIDs) {
  int error;
  TestCompletionCallback callback;
  ChannelIDService::Request request;

  std::string host1("encrypted.google.com");
  scoped_ptr<crypto::ECPrivateKey> key1;
  EXPECT_EQ(0, service_->channel_id_count());
  error = service_->GetOrCreateChannelID(host1, &key1, callback.callback(),
                                         &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());
  error = callback.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_EQ(1, service_->channel_id_count());

  std::string host2("www.verisign.com");
  scoped_ptr<crypto::ECPrivateKey> key2;
  error = service_->GetOrCreateChannelID(host2, &key2, callback.callback(),
                                         &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());
  error = callback.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_EQ(2, service_->channel_id_count());

  std::string host3("www.twitter.com");
  scoped_ptr<crypto::ECPrivateKey> key3;
  error = service_->GetOrCreateChannelID(host3, &key3, callback.callback(),
                                         &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());
  error = callback.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_EQ(3, service_->channel_id_count());

  EXPECT_FALSE(KeysEqual(key1.get(), key2.get()));
  EXPECT_FALSE(KeysEqual(key1.get(), key3.get()));
  EXPECT_FALSE(KeysEqual(key2.get(), key3.get()));
}

// Tests an inflight join.
TEST_F(ChannelIDServiceTest, InflightJoin) {
  std::string host("encrypted.google.com");
  int error;

  scoped_ptr<crypto::ECPrivateKey> key1;
  TestCompletionCallback callback1;
  ChannelIDService::Request request1;

  scoped_ptr<crypto::ECPrivateKey> key2;
  TestCompletionCallback callback2;
  ChannelIDService::Request request2;

  error = service_->GetOrCreateChannelID(host, &key1, callback1.callback(),
                                         &request1);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request1.is_active());
  // Should join with the original request.
  error = service_->GetOrCreateChannelID(host, &key2, callback2.callback(),
                                         &request2);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request2.is_active());

  error = callback1.WaitForResult();
  EXPECT_EQ(OK, error);
  error = callback2.WaitForResult();
  EXPECT_EQ(OK, error);

  EXPECT_EQ(2u, service_->requests());
  EXPECT_EQ(0u, service_->key_store_hits());
  EXPECT_EQ(1u, service_->inflight_joins());
  EXPECT_EQ(1u, service_->workers_created());
}

// Tests an inflight join of a Get request to a GetOrCreate request.
TEST_F(ChannelIDServiceTest, InflightJoinGetOrCreateAndGet) {
  std::string host("encrypted.google.com");
  int error;

  scoped_ptr<crypto::ECPrivateKey> key1;
  TestCompletionCallback callback1;
  ChannelIDService::Request request1;

  scoped_ptr<crypto::ECPrivateKey> key2;
  TestCompletionCallback callback2;
  ChannelIDService::Request request2;

  error = service_->GetOrCreateChannelID(host, &key1, callback1.callback(),
                                         &request1);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request1.is_active());
  // Should join with the original request.
  error = service_->GetChannelID(host, &key2, callback2.callback(), &request2);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request2.is_active());

  error = callback1.WaitForResult();
  EXPECT_EQ(OK, error);
  error = callback2.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_TRUE(KeysEqual(key1.get(), key2.get()));

  EXPECT_EQ(2u, service_->requests());
  EXPECT_EQ(0u, service_->key_store_hits());
  EXPECT_EQ(1u, service_->inflight_joins());
  EXPECT_EQ(1u, service_->workers_created());
}

// Tests that the callback of a canceled request is never made.
TEST_F(ChannelIDServiceTest, CancelRequest) {
  std::string host("encrypted.google.com");
  scoped_ptr<crypto::ECPrivateKey> key;
  int error;
  ChannelIDService::Request request;

  error = service_->GetOrCreateChannelID(host, &key, base::Bind(&FailTest),
                                         &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());
  request.Cancel();
  EXPECT_FALSE(request.is_active());

  // Wait for reply from ChannelIDServiceWorker to be posted back to the
  // ChannelIDService.
  base::MessageLoop::current()->RunUntilIdle();

  // Even though the original request was cancelled, the service will still
  // store the result, it just doesn't call the callback.
  EXPECT_EQ(1, service_->channel_id_count());
}

// Tests that destructing the Request cancels the request.
TEST_F(ChannelIDServiceTest, CancelRequestByHandleDestruction) {
  std::string host("encrypted.google.com");
  scoped_ptr<crypto::ECPrivateKey> key;
  int error;
  scoped_ptr<ChannelIDService::Request> request(
      new ChannelIDService::Request());

  error = service_->GetOrCreateChannelID(host, &key, base::Bind(&FailTest),
                                         request.get());
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request->is_active());

  // Delete the Request object.
  request.reset();

  // Wait for reply from ChannelIDServiceWorker to be posted back to the
  // ChannelIDService.
  base::MessageLoop::current()->RunUntilIdle();

  // Even though the original request was cancelled, the service will still
  // store the result, it just doesn't call the callback.
  EXPECT_EQ(1, service_->channel_id_count());
}

TEST_F(ChannelIDServiceTest, DestructionWithPendingRequest) {
  std::string host("encrypted.google.com");
  scoped_ptr<crypto::ECPrivateKey> key;
  int error;
  ChannelIDService::Request request;

  error = service_->GetOrCreateChannelID(host, &key, base::Bind(&FailTest),
                                         &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());

  // Cancel request and destroy the ChannelIDService.
  request.Cancel();
  service_.reset();

  // ChannelIDServiceWorker should not post anything back to the
  // non-existent ChannelIDService, but run the loop just to be sure it
  // doesn't.
  base::MessageLoop::current()->RunUntilIdle();

  // If we got here without crashing or a valgrind error, it worked.
}

// Tests that shutting down the sequenced worker pool and then making new
// requests gracefully fails.
// This is a regression test for http://crbug.com/236387
TEST_F(ChannelIDServiceTest, RequestAfterPoolShutdown) {
  scoped_refptr<FailingTaskRunner> task_runner(new FailingTaskRunner);
  service_.reset(new ChannelIDService(
      new DefaultChannelIDStore(NULL), task_runner));

  // Make a request that will force synchronous completion.
  std::string host("encrypted.google.com");
  scoped_ptr<crypto::ECPrivateKey> key;
  int error;
  ChannelIDService::Request request;

  error = service_->GetOrCreateChannelID(host, &key, base::Bind(&FailTest),
                                         &request);
  // If we got here without crashing or a valgrind error, it worked.
  ASSERT_EQ(ERR_INSUFFICIENT_RESOURCES, error);
  EXPECT_FALSE(request.is_active());
}

// Tests that simultaneous creation of different certs works.
TEST_F(ChannelIDServiceTest, SimultaneousCreation) {
  int error;

  std::string host1("encrypted.google.com");
  scoped_ptr<crypto::ECPrivateKey> key1;
  TestCompletionCallback callback1;
  ChannelIDService::Request request1;

  std::string host2("foo.com");
  scoped_ptr<crypto::ECPrivateKey> key2;
  TestCompletionCallback callback2;
  ChannelIDService::Request request2;

  std::string host3("bar.com");
  scoped_ptr<crypto::ECPrivateKey> key3;
  TestCompletionCallback callback3;
  ChannelIDService::Request request3;

  error = service_->GetOrCreateChannelID(host1, &key1, callback1.callback(),
                                         &request1);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request1.is_active());

  error = service_->GetOrCreateChannelID(host2, &key2, callback2.callback(),
                                         &request2);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request2.is_active());

  error = service_->GetOrCreateChannelID(host3, &key3, callback3.callback(),
                                         &request3);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request3.is_active());

  error = callback1.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_TRUE(key1);

  error = callback2.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_TRUE(key2);

  error = callback3.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_TRUE(key3);

  EXPECT_FALSE(KeysEqual(key1.get(), key2.get()));
  EXPECT_FALSE(KeysEqual(key1.get(), key3.get()));
  EXPECT_FALSE(KeysEqual(key2.get(), key3.get()));

  EXPECT_EQ(3, service_->channel_id_count());
}

TEST_F(ChannelIDServiceTest, AsyncStoreGetOrCreateNoChannelIDsInStore) {
  MockChannelIDStoreWithAsyncGet* mock_store =
      new MockChannelIDStoreWithAsyncGet();
  service_ = scoped_ptr<ChannelIDService>(new ChannelIDService(
      mock_store, base::MessageLoopProxy::current()));

  std::string host("encrypted.google.com");

  int error;
  TestCompletionCallback callback;
  ChannelIDService::Request request;

  // Asynchronous completion with no certs in the store.
  scoped_ptr<crypto::ECPrivateKey> key;
  EXPECT_EQ(0, service_->channel_id_count());
  error =
      service_->GetOrCreateChannelID(host, &key, callback.callback(), &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());

  mock_store->CallGetChannelIDCallbackWithResult(ERR_FILE_NOT_FOUND, nullptr);

  error = callback.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_EQ(1, service_->channel_id_count());
  EXPECT_TRUE(key);
  EXPECT_FALSE(request.is_active());
}

TEST_F(ChannelIDServiceTest, AsyncStoreGetNoChannelIDsInStore) {
  MockChannelIDStoreWithAsyncGet* mock_store =
      new MockChannelIDStoreWithAsyncGet();
  service_ = scoped_ptr<ChannelIDService>(new ChannelIDService(
      mock_store, base::MessageLoopProxy::current()));

  std::string host("encrypted.google.com");

  int error;
  TestCompletionCallback callback;
  ChannelIDService::Request request;

  // Asynchronous completion with no certs in the store.
  scoped_ptr<crypto::ECPrivateKey> key;
  EXPECT_EQ(0, service_->channel_id_count());
  error = service_->GetChannelID(host, &key, callback.callback(), &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());

  mock_store->CallGetChannelIDCallbackWithResult(ERR_FILE_NOT_FOUND, nullptr);

  error = callback.WaitForResult();
  EXPECT_EQ(ERR_FILE_NOT_FOUND, error);
  EXPECT_EQ(0, service_->channel_id_count());
  EXPECT_EQ(0u, service_->workers_created());
  EXPECT_FALSE(key);
  EXPECT_FALSE(request.is_active());
}

TEST_F(ChannelIDServiceTest, AsyncStoreGetOrCreateOneCertInStore) {
  MockChannelIDStoreWithAsyncGet* mock_store =
      new MockChannelIDStoreWithAsyncGet();
  service_ = scoped_ptr<ChannelIDService>(new ChannelIDService(
      mock_store, base::MessageLoopProxy::current()));

  std::string host("encrypted.google.com");

  int error;
  TestCompletionCallback callback;
  ChannelIDService::Request request;

  // Asynchronous completion with a cert in the store.
  scoped_ptr<crypto::ECPrivateKey> key;
  EXPECT_EQ(0, service_->channel_id_count());
  error =
      service_->GetOrCreateChannelID(host, &key, callback.callback(), &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());

  scoped_ptr<crypto::ECPrivateKey> expected_key(crypto::ECPrivateKey::Create());
  mock_store->CallGetChannelIDCallbackWithResult(OK, expected_key.get());

  error = callback.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_EQ(1, service_->channel_id_count());
  EXPECT_EQ(1u, service_->requests());
  EXPECT_EQ(1u, service_->key_store_hits());
  // Because the cert was found in the store, no new workers should have been
  // created.
  EXPECT_EQ(0u, service_->workers_created());
  EXPECT_TRUE(key);
  EXPECT_TRUE(KeysEqual(expected_key.get(), key.get()));
  EXPECT_FALSE(request.is_active());
}

TEST_F(ChannelIDServiceTest, AsyncStoreGetOneCertInStore) {
  MockChannelIDStoreWithAsyncGet* mock_store =
      new MockChannelIDStoreWithAsyncGet();
  service_ = scoped_ptr<ChannelIDService>(new ChannelIDService(
      mock_store, base::MessageLoopProxy::current()));

  std::string host("encrypted.google.com");

  int error;
  TestCompletionCallback callback;
  ChannelIDService::Request request;

  // Asynchronous completion with a cert in the store.
  scoped_ptr<crypto::ECPrivateKey> key;
  std::string private_key, spki;
  EXPECT_EQ(0, service_->channel_id_count());
  error = service_->GetChannelID(host, &key, callback.callback(), &request);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request.is_active());

  scoped_ptr<crypto::ECPrivateKey> expected_key(crypto::ECPrivateKey::Create());
  mock_store->CallGetChannelIDCallbackWithResult(OK, expected_key.get());

  error = callback.WaitForResult();
  EXPECT_EQ(OK, error);
  EXPECT_EQ(1, service_->channel_id_count());
  EXPECT_EQ(1u, service_->requests());
  EXPECT_EQ(1u, service_->key_store_hits());
  // Because the cert was found in the store, no new workers should have been
  // created.
  EXPECT_EQ(0u, service_->workers_created());
  EXPECT_TRUE(KeysEqual(expected_key.get(), key.get()));
  EXPECT_FALSE(request.is_active());
}

TEST_F(ChannelIDServiceTest, AsyncStoreGetThenCreateNoCertsInStore) {
  MockChannelIDStoreWithAsyncGet* mock_store =
      new MockChannelIDStoreWithAsyncGet();
  service_ = scoped_ptr<ChannelIDService>(new ChannelIDService(
      mock_store, base::MessageLoopProxy::current()));

  std::string host("encrypted.google.com");

  int error;

  // Asynchronous get with no certs in the store.
  TestCompletionCallback callback1;
  ChannelIDService::Request request1;
  scoped_ptr<crypto::ECPrivateKey> key1;
  EXPECT_EQ(0, service_->channel_id_count());
  error = service_->GetChannelID(host, &key1, callback1.callback(), &request1);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request1.is_active());

  // Asynchronous get/create with no certs in the store.
  TestCompletionCallback callback2;
  ChannelIDService::Request request2;
  scoped_ptr<crypto::ECPrivateKey> key2;
  EXPECT_EQ(0, service_->channel_id_count());
  error = service_->GetOrCreateChannelID(host, &key2, callback2.callback(),
                                         &request2);
  EXPECT_EQ(ERR_IO_PENDING, error);
  EXPECT_TRUE(request2.is_active());

  mock_store->CallGetChannelIDCallbackWithResult(ERR_FILE_NOT_FOUND, nullptr);

  // Even though the first request didn't ask to create a cert, it gets joined
  // by the second, which does, so both succeed.
  error = callback1.WaitForResult();
  EXPECT_EQ(OK, error);
  error = callback2.WaitForResult();
  EXPECT_EQ(OK, error);

  // One cert is created, one request is joined.
  EXPECT_EQ(2U, service_->requests());
  EXPECT_EQ(1, service_->channel_id_count());
  EXPECT_EQ(1u, service_->workers_created());
  EXPECT_EQ(1u, service_->inflight_joins());
  EXPECT_TRUE(key1);
  EXPECT_TRUE(KeysEqual(key1.get(), key2.get()));
  EXPECT_FALSE(request1.is_active());
  EXPECT_FALSE(request2.is_active());
}

}  // namespace

}  // namespace net
