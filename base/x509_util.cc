// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/x509_util.h"

#include "base/time.h"
#include "net/base/x509_certificate.h"

namespace net {

namespace x509_util {

bool ClientCertSorter::operator()(
    const scoped_refptr<X509Certificate>& a,
    const scoped_refptr<X509Certificate>& b) const {
  // Certificates that are null are sorted last.
  if (!a.get() || !b.get())
    return a.get() && !b.get();

  // Certificates that are expired/not-yet-valid are sorted last.
  base::Time now = base::Time::Now();
  bool a_is_valid = now >= a->valid_start() && now <= a->valid_expiry();
  bool b_is_valid = now >= b->valid_start() && now <= b->valid_expiry();
  if (a_is_valid && !b_is_valid)
    return true;

  // Certificates with longer expirations appear as higher priority (less
  // than) certificates with shorter expirations.
  if (a->valid_expiry() != b->valid_expiry())
    return a->valid_expiry() > b->valid_expiry();

  // If the expiration dates are equivalent, certificates that were issued
  // more recently should be prioritized over older certificates.
  if (a->valid_start() != b->valid_start())
    return a->valid_start() > b->valid_start();

  // Otherwise, prefer client certificates with shorter chains.
  const X509Certificate::OSCertHandles& a_intermediates =
      a->GetIntermediateCertificates();
  const X509Certificate::OSCertHandles& b_intermediates =
      b->GetIntermediateCertificates();
  return a_intermediates.size() < b_intermediates.size();
}

}  // namespace x509_util

}  // namespace net
