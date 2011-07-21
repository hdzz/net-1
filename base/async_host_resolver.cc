// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/async_host_resolver.h"

#include <algorithm>

#include "base/bind.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/stl_util.h"
#include "base/values.h"
#include "net/base/address_list.h"
#include "net/base/dns_util.h"
#include "net/base/net_errors.h"
#include "net/socket/client_socket_factory.h"

namespace net {

namespace {

// TODO(agayev): fix this when IPv6 support is added.
uint16 QueryTypeFromAddressFamily(AddressFamily address_family) {
  return kDNS_A;
}

int ResolveAsIp(const HostResolver::RequestInfo& info,
                const IPAddressNumber& ip_number,
                AddressList* addresses) {
  if (ip_number.size() != kIPv4AddressSize)
    return ERR_NAME_NOT_RESOLVED;

  *addresses = AddressList::CreateFromIPAddressWithCname(
      ip_number,
      info.port(),
      info.host_resolver_flags() & HOST_RESOLVER_CANONNAME);
  return OK;
}

class RequestParameters : public NetLog::EventParameters {
 public:
  RequestParameters(const HostResolver::RequestInfo& info,
                    const NetLog::Source& source)
      : info_(info), source_(source) {}

  virtual Value* ToValue() const {
    DictionaryValue* dict = new DictionaryValue();
    dict->SetString("hostname", info_.host_port_pair().ToString());
    dict->SetInteger("address_family",
                     static_cast<int>(info_.address_family()));
    dict->SetBoolean("allow_cached_response", info_.allow_cached_response());
    dict->SetBoolean("only_use_cached_response",
                     info_.only_use_cached_response());
    dict->SetBoolean("is_speculative", info_.is_speculative());
    dict->SetInteger("priority", info_.priority());

    if (source_.is_valid())
      dict->Set("source_dependency", source_.ToValue());

    return dict;
  }

 private:
  const HostResolver::RequestInfo info_;
  const NetLog::Source source_;
};

}  // namespace

HostResolver* CreateAsyncHostResolver(size_t max_concurrent_resolves,
                                      const IPAddressNumber& dns_ip,
                                      NetLog* net_log) {
  size_t max_transactions = max_concurrent_resolves;
  if (max_transactions == 0)
    max_transactions = 20;
  size_t max_pending_requests = max_transactions * 100;
  HostResolver* resolver = new AsyncHostResolver(
      IPEndPoint(dns_ip, 53),
      max_transactions,
      max_pending_requests,
      base::Bind(&base::RandInt),
      NULL,
      net_log);
  return resolver;
}

//-----------------------------------------------------------------------------
class AsyncHostResolver::Request {
 public:
  Request(const BoundNetLog& source_net_log,
          const BoundNetLog& request_net_log,
          int id,
          const HostResolver::RequestInfo& info,
          const Key& key,
          CompletionCallback* callback,
          AddressList* addresses)
      : source_net_log_(source_net_log),
        request_net_log_(request_net_log),
        id_(id),
        info_(info),
        key_(key),
        callback_(callback),
        addresses_(addresses) {
    DCHECK(addresses_);
  }

  int id() const { return id_; }
  const HostResolver::RequestInfo& info() const { return info_; }
  const Key& key() const { return key_; }
  RequestPriority priority() const { return info_.priority(); }
  const BoundNetLog& source_net_log() const { return source_net_log_; }
  const BoundNetLog& request_net_log() const { return request_net_log_; }
  const AddressList* addresses() const { return addresses_; }

  void OnComplete(int result, const IPAddressList& ip_addresses) {
    DCHECK(callback_);
    if (result == OK)
      *addresses_ =
          AddressList::CreateFromIPAddressList(ip_addresses, info_.port());
    callback_->Run(result);
  }

 private:
  BoundNetLog source_net_log_;
  BoundNetLog request_net_log_;
  const int id_;
  const HostResolver::RequestInfo info_;
  const Key key_;
  CompletionCallback* callback_;
  AddressList* addresses_;
};

//-----------------------------------------------------------------------------
AsyncHostResolver::AsyncHostResolver(const IPEndPoint& dns_server,
                                     size_t max_transactions,
                                     size_t max_pending_requests,
                                     const RandIntCallback& rand_int_cb,
                                     ClientSocketFactory* factory,
                                     NetLog* net_log)
    : max_transactions_(max_transactions),
      max_pending_requests_(max_pending_requests),
      dns_server_(dns_server),
      rand_int_cb_(rand_int_cb),
      factory_(factory),
      next_request_id_(0),
      net_log_(net_log) {
}

AsyncHostResolver::~AsyncHostResolver() {
  // Destroy request lists.
  for (KeyRequestListMap::iterator it = requestlist_map_.begin();
       it != requestlist_map_.end(); ++it)
    STLDeleteElements(&it->second);

  // Destroy transactions.
  STLDeleteElements(&transactions_);

  // Destroy pending requests.
  for (size_t i = 0; i < arraysize(pending_requests_); ++i)
    STLDeleteElements(&pending_requests_[i]);
}

int AsyncHostResolver::Resolve(const RequestInfo& info,
                               AddressList* addresses,
                               CompletionCallback* callback,
                               RequestHandle* out_req,
                               const BoundNetLog& source_net_log) {
  DCHECK(addresses);
  IPAddressNumber ip_number;
  std::string dns_name;
  int rv = ERR_UNEXPECTED;
  if (info.hostname().empty())
    rv = ERR_NAME_NOT_RESOLVED;
  else if (ParseIPLiteralToNumber(info.hostname(), &ip_number))
    rv = ResolveAsIp(info, ip_number, addresses);
  else if (!DNSDomainFromDot(info.hostname(), &dns_name))
    rv = ERR_NAME_NOT_RESOLVED;
  else if (info.only_use_cached_response())  // TODO(agayev): support caching
    rv = ERR_NAME_NOT_RESOLVED;

  Request* request = CreateNewRequest(
      info, dns_name, callback, addresses, source_net_log);

  OnStart(request);
  if (rv != ERR_UNEXPECTED) {
    OnFinish(request, rv);
    delete request;
    return rv;
  }

  if (out_req)
    *out_req = reinterpret_cast<RequestHandle>(request);

  if (AttachToRequestList(request))
    return ERR_IO_PENDING;
  if (transactions_.size() < max_transactions_)
    return StartNewTransactionFor(request);
  return Enqueue(request);
}

void AsyncHostResolver::OnStart(Request* request) {
  DCHECK(request);

  request->source_net_log().BeginEvent(
      NetLog::TYPE_ASYNC_HOST_RESOLVER,
      make_scoped_refptr(new NetLogSourceParameter(
          "source_dependency", request->request_net_log().source())));

  request->request_net_log().BeginEvent(
      NetLog::TYPE_ASYNC_HOST_RESOLVER_REQUEST,
      make_scoped_refptr(new RequestParameters(
          request->info(), request->source_net_log().source())));

  FOR_EACH_OBSERVER(
      HostResolver::Observer, observers_,
      OnStartResolution(request->id(), request->info()));
}

void AsyncHostResolver::OnFinish(Request* request, int result) {
  DCHECK(request);
  bool was_resolved = result == OK;

  FOR_EACH_OBSERVER(
      HostResolver::Observer, observers_,
      OnFinishResolutionWithStatus(
          request->id(), was_resolved, request->info()));

  request->request_net_log().EndEventWithNetErrorCode(
      NetLog::TYPE_ASYNC_HOST_RESOLVER_REQUEST, result);
  request->source_net_log().EndEvent(
      NetLog::TYPE_ASYNC_HOST_RESOLVER, NULL);
}

void AsyncHostResolver::OnCancel(Request* request) {
  DCHECK(request);

  FOR_EACH_OBSERVER(
      HostResolver::Observer, observers_,
      OnCancelResolution(request->id(), request->info()));

  request->request_net_log().AddEvent(
      NetLog::TYPE_CANCELLED, NULL);
  request->request_net_log().EndEvent(
      NetLog::TYPE_ASYNC_HOST_RESOLVER_REQUEST, NULL);
  request->source_net_log().EndEvent(
      NetLog::TYPE_ASYNC_HOST_RESOLVER, NULL);
}

void AsyncHostResolver::CancelRequest(RequestHandle req_handle) {
  scoped_ptr<Request> request(reinterpret_cast<Request*>(req_handle));
  DCHECK(request.get());

  KeyRequestListMap::iterator it = requestlist_map_.find(request->key());
  if (it != requestlist_map_.end())
    it->second.remove(request.get());
  else
    pending_requests_[request->priority()].remove(request.get());

  OnCancel(request.get());
}

void AsyncHostResolver::AddObserver(HostResolver::Observer* observer) {
  observers_.AddObserver(observer);
}

void AsyncHostResolver::RemoveObserver(HostResolver::Observer* observer) {
  observers_.RemoveObserver(observer);
}

void AsyncHostResolver::SetDefaultAddressFamily(
    AddressFamily address_family) {
  NOTIMPLEMENTED();
}

AddressFamily AsyncHostResolver::GetDefaultAddressFamily() const {
  return ADDRESS_FAMILY_IPV4;
}

HostResolverImpl* AsyncHostResolver::GetAsHostResolverImpl() {
  return NULL;
}

void AsyncHostResolver::OnTransactionComplete(
    int result,
    const DnsTransaction* transaction,
    const IPAddressList& ip_addresses) {

  DCHECK(std::find(transactions_.begin(), transactions_.end(), transaction)
         != transactions_.end());
  DCHECK(requestlist_map_.find(transaction->key()) != requestlist_map_.end());

  // Run callback of every request that was depending on this transaction,
  // also notify observers.
  RequestList& requests = requestlist_map_[transaction->key()];
  for (RequestList::iterator it = requests.begin(); it != requests.end();
       ++it) {
    Request* request = *it;
    OnFinish(request, result);
    request->OnComplete(result, ip_addresses);
  }

  // Cleanup requests.
  STLDeleteElements(&requests);
  requestlist_map_.erase(transaction->key());

  // Cleanup transaction and start a new one if there are pending requests.
  delete transaction;
  transactions_.remove(transaction);
  ProcessPending();
}

AsyncHostResolver::Request* AsyncHostResolver::CreateNewRequest(
    const RequestInfo& info,
    const std::string& dns_name,
    CompletionCallback* callback,
    AddressList* addresses,
    const BoundNetLog& source_net_log) {

  uint16 query_type = QueryTypeFromAddressFamily(info.address_family());
  Key key(dns_name, query_type);

  BoundNetLog request_net_log = BoundNetLog::Make(net_log_,
      NetLog::SOURCE_ASYNC_HOST_RESOLVER_REQUEST);

  int id = next_request_id_++;
  return new Request(
      source_net_log, request_net_log, id, info, key, callback, addresses);
}

bool AsyncHostResolver::AttachToRequestList(Request* request) {
  KeyRequestListMap::iterator it = requestlist_map_.find(request->key());
  if (it == requestlist_map_.end())
    return false;
  it->second.push_back(request);
  return true;
}

int AsyncHostResolver::StartNewTransactionFor(Request* request) {
  DCHECK(requestlist_map_.find(request->key()) == requestlist_map_.end());
  DCHECK(transactions_.size() < max_transactions_);

  request->request_net_log().AddEvent(
      NetLog::TYPE_ASYNC_HOST_RESOLVER_CREATE_DNS_TRANSACTION, NULL);

  requestlist_map_[request->key()].push_back(request);
  DnsTransaction* transaction = new DnsTransaction(
      dns_server_,
      request->key().first,
      request->key().second,
      rand_int_cb_,
      factory_,
      request->request_net_log(),
      net_log_);
  transaction->SetDelegate(this);
  transactions_.push_back(transaction);
  return transaction->Start();
}

int AsyncHostResolver::Enqueue(Request* request) {
  scoped_ptr<Request> evicted_request(Insert(request));
  if (evicted_request != NULL) {
    int rv = ERR_HOST_RESOLVER_QUEUE_TOO_LARGE;
    if (evicted_request.get() == request)
      return rv;
    evicted_request->OnComplete(rv, IPAddressList());
  }
  return ERR_IO_PENDING;
}

AsyncHostResolver::Request* AsyncHostResolver::Insert(Request* request) {
  pending_requests_[request->priority()].push_back(request);
  if (GetNumPending() > max_pending_requests_) {
    Request* req = RemoveLowest();
    DCHECK(req);
    return req;
  }
  return NULL;
}

size_t AsyncHostResolver::GetNumPending() {
  size_t num_pending = 0;
  for (size_t i = 0; i < arraysize(pending_requests_); ++i)
    num_pending += pending_requests_[i].size();
  return num_pending;
}

AsyncHostResolver::Request* AsyncHostResolver::RemoveLowest() {
  for (int i = static_cast<int>(arraysize(pending_requests_)) - 1;
       i >= 0; --i) {
    RequestList& requests = pending_requests_[i];
    if (!requests.empty()) {
      Request* request = requests.front();
      requests.pop_front();
      return request;
    }
  }
  return NULL;
}

AsyncHostResolver::Request* AsyncHostResolver::RemoveHighest() {
  for (size_t i = 0; i < arraysize(pending_requests_) - 1; ++i) {
    RequestList& requests = pending_requests_[i];
    if (!requests.empty()) {
      Request* request = requests.front();
      requests.pop_front();
      return request;
    }
  }
  return NULL;
}

void AsyncHostResolver::ProcessPending() {
  Request* request = RemoveHighest();
  if (!request)
    return;
  for (size_t i = 0; i < arraysize(pending_requests_); ++i) {
    RequestList& requests = pending_requests_[i];
    RequestList::iterator it = requests.begin();
    while (it != requests.end()) {
      if (request->key() == (*it)->key()) {
        requestlist_map_[request->key()].push_back(*it);
        it = requests.erase(it);
      } else {
        ++it;
      }
    }
  }
  StartNewTransactionFor(request);
}

}  // namespace net
