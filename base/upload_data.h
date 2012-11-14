// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_UPLOAD_DATA_H_
#define NET_BASE_UPLOAD_DATA_H_

#include "base/basictypes.h"
#include "base/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_vector.h"
#include "base/supports_user_data.h"
#include "net/base/net_export.h"
#include "net/base/upload_element.h"

class FilePath;

namespace base {
class Time;
}  // namespace base

namespace net {

//-----------------------------------------------------------------------------
// A very concrete class representing the data to be uploaded as part of a
// URLRequest.
//
// Until there is a more abstract class for this, this one derives from
// SupportsUserData to allow users to stash random data by
// key and ensure its destruction when UploadData is finally deleted.
//
// Chunked uploads are handled by repeatedly calling AppendChunk() as data
// becomes available, which adds to |elements_|.  Whenever this happens,
// |chunk_callback_| is called, if non-NULL.
class NET_EXPORT UploadData
    : public base::RefCounted<UploadData>,
      public base::SupportsUserData {
 public:
  UploadData();

  void AppendBytes(const char* bytes, int bytes_len);

  void AppendFileRange(const FilePath& file_path,
                       uint64 offset, uint64 length,
                       const base::Time& expected_modification_time);

  // Adds the given chunk of bytes to be sent immediately with chunked transfer
  // encoding.
  void AppendChunk(const char* bytes, int bytes_len, bool is_last_chunk);

  // Sets the callback to be invoked when a new chunk is available to upload.
  void set_chunk_callback(const base::Closure& callback);

  // Initializes the object to send chunks of upload data over time rather
  // than all at once. Chunked data may only contain bytes, not files.
  void set_is_chunked(bool set) { is_chunked_ = set; }
  bool is_chunked() const { return is_chunked_; }

  // set_last_chunk_appended() is only used for serialization.
  void set_last_chunk_appended(bool set) { last_chunk_appended_ = set; }
  bool last_chunk_appended() const { return last_chunk_appended_; }

  const ScopedVector<UploadElement>& elements() const {
    return elements_;
  }

  ScopedVector<UploadElement>* elements_mutable() {
    return &elements_;
  }

  void swap_elements(ScopedVector<UploadElement>* elements) {
    elements_.swap(*elements);
  }

  // Identifies a particular upload instance, which is used by the cache to
  // formulate a cache key.  This value should be unique across browser
  // sessions.  A value of 0 is used to indicate an unspecified identifier.
  void set_identifier(int64 id) { identifier_ = id; }
  int64 identifier() const { return identifier_; }

 private:
  friend class base::RefCounted<UploadData>;

  virtual ~UploadData();

  ScopedVector<UploadElement> elements_;
  int64 identifier_;
  base::Closure chunk_callback_;
  bool is_chunked_;
  bool last_chunk_appended_;

  DISALLOW_COPY_AND_ASSIGN(UploadData);
};

}  // namespace net

#endif  // NET_BASE_UPLOAD_DATA_H_
