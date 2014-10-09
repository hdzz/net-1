// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_URL_REQUEST_URL_REQUEST_FTP_JOB_H_
#define NET_URL_REQUEST_URL_REQUEST_FTP_JOB_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "net/base/auth.h"
#include "net/base/net_export.h"
#include "net/ftp/ftp_request_info.h"
#include "net/ftp/ftp_transaction.h"
#include "net/http/http_request_info.h"
#include "net/http/http_transaction.h"
#include "net/proxy/proxy_info.h"
#include "net/proxy/proxy_service.h"
#include "net/url_request/url_request_job.h"

namespace net {

class NetworkDelegate;
class FtpTransactionFactory;
class FtpAuthCache;

// A URLRequestJob subclass that is built on top of FtpTransaction. It
// provides an implementation for FTP.
class NET_EXPORT_PRIVATE URLRequestFtpJob : public URLRequestJob {
 public:
  URLRequestFtpJob(URLRequest* request,
                   NetworkDelegate* network_delegate,
                   FtpTransactionFactory* ftp_transaction_factory,
                   FtpAuthCache* ftp_auth_cache);

 protected:
  virtual ~URLRequestFtpJob();

  // Overridden from URLRequestJob:
  virtual bool IsSafeRedirect(const GURL& location) override;
  virtual bool GetMimeType(std::string* mime_type) const override;
  virtual void GetResponseInfo(HttpResponseInfo* info) override;
  virtual HostPortPair GetSocketAddress() const override;
  virtual void SetPriority(RequestPriority priority) override;
  virtual void Start() override;
  virtual void Kill() override;

  RequestPriority priority() const { return priority_; }

 private:
  void OnResolveProxyComplete(int result);

  void StartFtpTransaction();
  void StartHttpTransaction();

  void OnStartCompleted(int result);
  void OnStartCompletedAsync(int result);
  void OnReadCompleted(int result);

  void RestartTransactionWithAuth();

  void LogFtpServerType(char server_type);

  // Overridden from URLRequestJob:
  virtual LoadState GetLoadState() const override;
  virtual bool NeedsAuth() override;
  virtual void GetAuthChallengeInfo(
      scoped_refptr<AuthChallengeInfo>* auth_info) override;
  virtual void SetAuth(const AuthCredentials& credentials) override;
  virtual void CancelAuth() override;

  // TODO(ibrar):  Yet to give another look at this function.
  virtual UploadProgress GetUploadProgress() const override;
  virtual bool ReadRawData(IOBuffer* buf,
                           int buf_size,
                           int *bytes_read) override;

  void HandleAuthNeededResponse();

  RequestPriority priority_;

  ProxyService* proxy_service_;
  ProxyInfo proxy_info_;
  ProxyService::PacRequest* pac_request_;

  FtpRequestInfo ftp_request_info_;
  scoped_ptr<FtpTransaction> ftp_transaction_;

  HttpRequestInfo http_request_info_;
  scoped_ptr<HttpTransaction> http_transaction_;
  const HttpResponseInfo* http_response_info_;

  bool read_in_progress_;

  scoped_refptr<AuthData> auth_data_;

  FtpTransactionFactory* ftp_transaction_factory_;
  FtpAuthCache* ftp_auth_cache_;

  base::WeakPtrFactory<URLRequestFtpJob> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(URLRequestFtpJob);
};

}  // namespace net

#endif  // NET_URL_REQUEST_URL_REQUEST_FTP_JOB_H_
