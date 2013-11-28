// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/multi_log_ct_verifier.h"

#include "net/base/net_errors.h"
#include "net/cert/ct_log_verifier.h"
#include "net/cert/ct_objects_extractor.h"
#include "net/cert/ct_serialization.h"
#include "net/cert/ct_verify_result.h"
#include "net/cert/x509_certificate.h"

namespace net {

MultiLogCTVerifier::MultiLogCTVerifier() { }

MultiLogCTVerifier::~MultiLogCTVerifier() { }

void MultiLogCTVerifier::AddLog(scoped_ptr<CTLogVerifier> log_verifier) {
  DCHECK(log_verifier);
  if (!log_verifier)
    return;

  linked_ptr<CTLogVerifier> log(log_verifier.release());
  logs_[log->key_id()] = log;
}

int MultiLogCTVerifier::Verify(
    X509Certificate* cert,
    const std::string& sct_list_from_ocsp,
    const std::string& sct_list_from_tls_extension,
    ct::CTVerifyResult* result)  {
  DCHECK(cert);
  DCHECK(result);

  result->verified_scts.clear();
  result->unverified_scts.clear();
  result->unknown_logs_scts.clear();

  bool has_verified_scts = false;

  std::string embedded_scts;
  if (!cert->GetIntermediateCertificates().empty() &&
      ct::ExtractEmbeddedSCTList(
          cert->os_cert_handle(),
          &embedded_scts)) {
    ct::LogEntry precert_entry;

    has_verified_scts =
        ct::GetPrecertLogEntry(
            cert->os_cert_handle(),
            cert->GetIntermediateCertificates().front(),
            &precert_entry) &&
        VerifySCTs(
            embedded_scts,
            precert_entry,
            ct::SignedCertificateTimestamp::SCT_EMBEDDED,
            result);
  }

  ct::LogEntry x509_entry;
  if (ct::GetX509LogEntry(cert->os_cert_handle(), &x509_entry)) {
    has_verified_scts |= VerifySCTs(
        sct_list_from_ocsp,
        x509_entry,
        ct::SignedCertificateTimestamp::SCT_FROM_OCSP_RESPONSE,
        result);

    has_verified_scts |= VerifySCTs(
        sct_list_from_tls_extension,
        x509_entry,
        ct::SignedCertificateTimestamp::SCT_FROM_TLS_EXTENSION,
        result);
  }

  if (has_verified_scts)
    return OK;

  return ERR_CT_NO_SCTS_VERIFIED_OK;
}

bool MultiLogCTVerifier::VerifySCTs(
    const std::string& encoded_sct_list,
    const ct::LogEntry& expected_entry,
    ct::SignedCertificateTimestamp::Origin origin,
    ct::CTVerifyResult* result) {
  if (logs_.empty())
    return false;

  base::StringPiece temp(encoded_sct_list);
  std::vector<base::StringPiece> sct_list;

  if (!ct::DecodeSCTList(&temp, &sct_list))
    return false;

  bool verified = false;
  for (std::vector<base::StringPiece>::const_iterator it = sct_list.begin();
       it != sct_list.end(); ++it) {
    base::StringPiece encoded_sct(*it);

    scoped_refptr<ct::SignedCertificateTimestamp> decoded_sct;
    if (!DecodeSignedCertificateTimestamp(&encoded_sct, &decoded_sct)) {
      // XXX(rsleevi): Should we really just skip over bad SCTs?
      continue;
    }
    decoded_sct->origin = origin;

    verified |= VerifySingleSCT(decoded_sct, expected_entry, result);
  }

  return verified;
}

bool MultiLogCTVerifier::VerifySingleSCT(
    scoped_refptr<ct::SignedCertificateTimestamp> sct,
    const ct::LogEntry& expected_entry,
    ct::CTVerifyResult* result) {

  // Assume this SCT is untrusted until proven otherwise.
  IDToLogMap::iterator it = logs_.find(sct->log_id);
  if (it == logs_.end()) {
    DVLOG(1) << "SCT does not match any known log.";
    result->unknown_logs_scts.push_back(sct);
    return false;
  }

  if (!it->second->Verify(expected_entry, *sct)) {
    DVLOG(1) << "Unable to verify SCT signature.";
    result->unverified_scts.push_back(sct);
    return false;
  }

  // SCT verified ok, just make sure the timestamp is legitimate.
  if (sct->timestamp > base::Time::Now()) {
    DVLOG(1) << "SCT is from the future!";
    result->unverified_scts.push_back(sct);
    return false;
  }

  result->verified_scts.push_back(sct);
  return true;
}

} // namespace net
