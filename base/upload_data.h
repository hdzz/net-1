// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_UPLOAD_DATA_H_
#define NET_BASE_UPLOAD_DATA_H_

#include <vector>

#include "base/basictypes.h"
#include "base/callback_forward.h"
#include "base/memory/ref_counted.h"
#include "base/supports_user_data.h"
#include "net/base/net_export.h"
#include "net/base/upload_element.h"

class FilePath;
class GURL;

namespace base {
class Time;
}  // namespace base

namespace net {

// Interface implemented by callers who require callbacks when new chunks
// of data are added.
class NET_EXPORT_PRIVATE ChunkCallback {
 public:
  // Invoked when a new data chunk was given for a chunked transfer upload.
  virtual void OnChunkAvailable() = 0;

 protected:
  virtual ~ChunkCallback() {}
};

//-----------------------------------------------------------------------------
// A very concrete class representing the data to be uploaded as part of a
// URLRequest.
//
// Until there is a more abstract class for this, this one derives from
// SupportsUserData to allow users to stash random data by
// key and ensure its destruction when UploadData is finally deleted.
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
  void set_chunk_callback(ChunkCallback* callback);

  // Initializes the object to send chunks of upload data over time rather
  // than all at once.
  void set_is_chunked(bool set) { is_chunked_ = set; }
  bool is_chunked() const { return is_chunked_; }

  // Gets the total size in bytes of the data to upload. Computing the
  // content length can result in performing file IO hence the operation is
  // done asynchronously. Runs the callback with the content length once the
  // computation is done.
  typedef base::Callback<void(uint64 content_length)> ContentLengthCallback;
  void GetContentLength(const ContentLengthCallback& callback);

  // Returns the total size in bytes of the data to upload, for testing.
  // This version may perform file IO on the current thread. This function
  // will fail if called on a thread where file IO is prohibited. Usually
  // used for testing, but Chrome Frame also uses this version.
  uint64 GetContentLengthSync();

  // Returns true if the upload data is entirely in memory (i.e. the
  // upload data is not chunked, and all elemnts are of TYPE_BYTES).
  bool IsInMemory() const;

  // Resets the offset of each upload data element to zero, so that the
  // upload data can be reread. This can happen if the same upload data is
  // reused for a new UploadDataStream.
  void ResetOffset();

  const std::vector<UploadElement>* elements() const {
    return &elements_;
  }

  std::vector<UploadElement>* elements_mutable() {
    return &elements_;
  }

  void SetElements(const std::vector<UploadElement>& elements);

  void swap_elements(std::vector<UploadElement>* elements) {
    elements_.swap(*elements);
  }

  // Identifies a particular upload instance, which is used by the cache to
  // formulate a cache key.  This value should be unique across browser
  // sessions.  A value of 0 is used to indicate an unspecified identifier.
  void set_identifier(int64 id) { identifier_ = id; }
  int64 identifier() const { return identifier_; }

 private:
  // Helper function for GetContentLength().
  void DoGetContentLength(uint64* content_length);

  friend class base::RefCounted<UploadData>;

  virtual ~UploadData();

  std::vector<UploadElement> elements_;
  int64 identifier_;
  ChunkCallback* chunk_callback_;
  bool is_chunked_;

  DISALLOW_COPY_AND_ASSIGN(UploadData);
};

}  // namespace net

#endif  // NET_BASE_UPLOAD_DATA_H_
