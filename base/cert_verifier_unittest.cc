// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/cert_verifier.h"

#include "base/bind.h"
#include "base/file_path.h"
#include "base/format_macros.h"
#include "base/stringprintf.h"
#include "net/base/cert_test_util.h"
#include "net/base/net_errors.h"
#include "net/base/net_log.h"
#include "net/base/test_completion_callback.h"
#include "net/base/x509_certificate.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace {

void FailTest(int /* result */) {
  FAIL();
}

}  // namespace;

// Tests a cache hit, which should result in synchronous completion.
TEST(CertVerifierTest, CacheHit) {
  CertVerifier verifier;

  FilePath certs_dir = GetTestCertsDirectory();
  scoped_refptr<X509Certificate> test_cert(
      ImportCertFromFile(certs_dir, "ok_cert.pem"));
  ASSERT_NE(static_cast<X509Certificate*>(NULL), test_cert);

  int error;
  CertVerifyResult verify_result;
  TestCompletionCallback callback;
  CertVerifier::RequestHandle request_handle;

  error = verifier.Verify(test_cert, "www.example.com", 0, NULL, &verify_result,
                          callback.callback(), &request_handle, BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, error);
  ASSERT_TRUE(request_handle != NULL);
  error = callback.WaitForResult();
  ASSERT_TRUE(IsCertificateError(error));
  ASSERT_EQ(1u, verifier.requests());
  ASSERT_EQ(0u, verifier.cache_hits());
  ASSERT_EQ(0u, verifier.inflight_joins());
  ASSERT_EQ(1u, verifier.GetCacheSize());

  error = verifier.Verify(test_cert, "www.example.com", 0, NULL, &verify_result,
                          callback.callback(), &request_handle, BoundNetLog());
  // Synchronous completion.
  ASSERT_NE(ERR_IO_PENDING, error);
  ASSERT_TRUE(IsCertificateError(error));
  ASSERT_TRUE(request_handle == NULL);
  ASSERT_EQ(2u, verifier.requests());
  ASSERT_EQ(1u, verifier.cache_hits());
  ASSERT_EQ(0u, verifier.inflight_joins());
  ASSERT_EQ(1u, verifier.GetCacheSize());
}

// Tests the same server certificate with different intermediate CA
// certificates.  These should be treated as different certificate chains even
// though the two X509Certificate objects contain the same server certificate.
TEST(CertVerifierTest, DifferentCACerts) {
  CertVerifier verifier;

  FilePath certs_dir = GetTestCertsDirectory();

  scoped_refptr<X509Certificate> server_cert =
      ImportCertFromFile(certs_dir, "salesforce_com_test.pem");
  ASSERT_NE(static_cast<X509Certificate*>(NULL), server_cert);

  scoped_refptr<X509Certificate> intermediate_cert1 =
      ImportCertFromFile(certs_dir, "verisign_intermediate_ca_2011.pem");
  ASSERT_NE(static_cast<X509Certificate*>(NULL), intermediate_cert1);

  scoped_refptr<X509Certificate> intermediate_cert2 =
      ImportCertFromFile(certs_dir, "verisign_intermediate_ca_2016.pem");
  ASSERT_NE(static_cast<X509Certificate*>(NULL), intermediate_cert2);

  X509Certificate::OSCertHandles intermediates;
  intermediates.push_back(intermediate_cert1->os_cert_handle());
  scoped_refptr<X509Certificate> cert_chain1 =
      X509Certificate::CreateFromHandle(server_cert->os_cert_handle(),
                                        intermediates);

  intermediates.clear();
  intermediates.push_back(intermediate_cert2->os_cert_handle());
  scoped_refptr<X509Certificate> cert_chain2 =
      X509Certificate::CreateFromHandle(server_cert->os_cert_handle(),
                                        intermediates);

  int error;
  CertVerifyResult verify_result;
  TestCompletionCallback callback;
  CertVerifier::RequestHandle request_handle;

  error = verifier.Verify(cert_chain1, "www.example.com", 0, NULL,
                          &verify_result, callback.callback(),
                          &request_handle, BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, error);
  ASSERT_TRUE(request_handle != NULL);
  error = callback.WaitForResult();
  ASSERT_TRUE(IsCertificateError(error));
  ASSERT_EQ(1u, verifier.requests());
  ASSERT_EQ(0u, verifier.cache_hits());
  ASSERT_EQ(0u, verifier.inflight_joins());
  ASSERT_EQ(1u, verifier.GetCacheSize());

  error = verifier.Verify(cert_chain2, "www.example.com", 0, NULL,
                          &verify_result, callback.callback(),
                          &request_handle, BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, error);
  ASSERT_TRUE(request_handle != NULL);
  error = callback.WaitForResult();
  ASSERT_TRUE(IsCertificateError(error));
  ASSERT_EQ(2u, verifier.requests());
  ASSERT_EQ(0u, verifier.cache_hits());
  ASSERT_EQ(0u, verifier.inflight_joins());
  ASSERT_EQ(2u, verifier.GetCacheSize());
}

// Tests an inflight join.
TEST(CertVerifierTest, InflightJoin) {
  CertVerifier verifier;

  FilePath certs_dir = GetTestCertsDirectory();
  scoped_refptr<X509Certificate> test_cert(
      ImportCertFromFile(certs_dir, "ok_cert.pem"));
  ASSERT_NE(static_cast<X509Certificate*>(NULL), test_cert);

  int error;
  CertVerifyResult verify_result;
  TestCompletionCallback callback;
  CertVerifier::RequestHandle request_handle;
  CertVerifyResult verify_result2;
  TestCompletionCallback callback2;
  CertVerifier::RequestHandle request_handle2;

  error = verifier.Verify(test_cert, "www.example.com", 0, NULL, &verify_result,
                          callback.callback(), &request_handle, BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, error);
  ASSERT_TRUE(request_handle != NULL);
  error = verifier.Verify(
      test_cert, "www.example.com", 0, NULL, &verify_result2,
      callback2.callback(), &request_handle2, BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, error);
  ASSERT_TRUE(request_handle2 != NULL);
  error = callback.WaitForResult();
  ASSERT_TRUE(IsCertificateError(error));
  error = callback2.WaitForResult();
  ASSERT_TRUE(IsCertificateError(error));
  ASSERT_EQ(2u, verifier.requests());
  ASSERT_EQ(0u, verifier.cache_hits());
  ASSERT_EQ(1u, verifier.inflight_joins());
}

// Tests that the callback of a canceled request is never made.
TEST(CertVerifierTest, CancelRequest) {
  CertVerifier verifier;

  FilePath certs_dir = GetTestCertsDirectory();
  scoped_refptr<X509Certificate> test_cert(
      ImportCertFromFile(certs_dir, "ok_cert.pem"));
  ASSERT_NE(static_cast<X509Certificate*>(NULL), test_cert);

  int error;
  CertVerifyResult verify_result;
  CertVerifier::RequestHandle request_handle;

  error = verifier.Verify(
      test_cert, "www.example.com", 0, NULL, &verify_result,
      base::Bind(&FailTest), &request_handle, BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, error);
  ASSERT_TRUE(request_handle != NULL);
  verifier.CancelRequest(request_handle);

  // Issue a few more requests to the worker pool and wait for their
  // completion, so that the task of the canceled request (which runs on a
  // worker thread) is likely to complete by the end of this test.
  TestCompletionCallback callback;
  for (int i = 0; i < 5; ++i) {
    error = verifier.Verify(
        test_cert, "www2.example.com", 0, NULL, &verify_result,
        callback.callback(), &request_handle, BoundNetLog());
    ASSERT_EQ(ERR_IO_PENDING, error);
    ASSERT_TRUE(request_handle != NULL);
    error = callback.WaitForResult();
    verifier.ClearCache();
  }
}

// Tests that a canceled request is not leaked.
TEST(CertVerifierTest, CancelRequestThenQuit) {
  CertVerifier verifier;

  FilePath certs_dir = GetTestCertsDirectory();
  scoped_refptr<X509Certificate> test_cert(
      ImportCertFromFile(certs_dir, "ok_cert.pem"));
  ASSERT_NE(static_cast<X509Certificate*>(NULL), test_cert);

  int error;
  CertVerifyResult verify_result;
  TestCompletionCallback callback;
  CertVerifier::RequestHandle request_handle;

  error = verifier.Verify(test_cert, "www.example.com", 0, NULL, &verify_result,
                          callback.callback(), &request_handle, BoundNetLog());
  ASSERT_EQ(ERR_IO_PENDING, error);
  ASSERT_TRUE(request_handle != NULL);
  verifier.CancelRequest(request_handle);
  // Destroy |verifier| by going out of scope.
}

TEST(CertVerifierTest, RequestParamsComparators) {
  SHA1Fingerprint a_key;
  memset(a_key.data, 'a', sizeof(a_key.data));

  SHA1Fingerprint z_key;
  memset(z_key.data, 'z', sizeof(z_key.data));

  struct {
    // Keys to test
    CertVerifier::RequestParams key1;
    CertVerifier::RequestParams key2;

    // Expectation:
    // -1 means key1 is less than key2
    //  0 means key1 equals key2
    //  1 means key1 is greater than key2
    int expected_result;
  } tests[] = {
    {  // Test for basic equivalence.
      CertVerifier::RequestParams(a_key, a_key, "www.example.test", 0),
      CertVerifier::RequestParams(a_key, a_key, "www.example.test", 0),
      0,
    },
    {  // Test that different certificates but with the same CA and for
       // the same host are different validation keys.
      CertVerifier::RequestParams(a_key, a_key, "www.example.test", 0),
      CertVerifier::RequestParams(z_key, a_key, "www.example.test", 0),
      -1,
    },
    {  // Test that the same EE certificate for the same host, but with
       // different chains are different validation keys.
      CertVerifier::RequestParams(a_key, z_key, "www.example.test", 0),
      CertVerifier::RequestParams(a_key, a_key, "www.example.test", 0),
      1,
    },
    {  // The same certificate, with the same chain, but for different
       // hosts are different validation keys.
      CertVerifier::RequestParams(a_key, a_key, "www1.example.test", 0),
      CertVerifier::RequestParams(a_key, a_key, "www2.example.test", 0),
      -1,
    },
    {  // The same certificate, chain, and host, but with different flags
       // are different validation keys.
      CertVerifier::RequestParams(a_key, a_key, "www.example.test",
                                  X509Certificate::VERIFY_EV_CERT),
      CertVerifier::RequestParams(a_key, a_key, "www.example.test", 0),
      1,
    }
  };
  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(tests); ++i) {
    SCOPED_TRACE(base::StringPrintf("Test[%" PRIuS "]", i));

    const CertVerifier::RequestParams& key1 = tests[i].key1;
    const CertVerifier::RequestParams& key2 = tests[i].key2;

    switch (tests[i].expected_result) {
      case -1:
        EXPECT_TRUE(key1 < key2);
        EXPECT_FALSE(key2 < key1);
        break;
      case 0:
        EXPECT_FALSE(key1 < key2);
        EXPECT_FALSE(key2 < key1);
        break;
      case 1:
        EXPECT_FALSE(key1 < key2);
        EXPECT_TRUE(key2 < key1);
        break;
      default:
        FAIL() << "Invalid expectation. Can be only -1, 0, 1";
    }
  }
}

}  // namespace net
