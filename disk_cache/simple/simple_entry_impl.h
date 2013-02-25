// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_DISK_CACHE_SIMPLE_SIMPLE_ENTRY_IMPL_H_
#define NET_DISK_CACHE_SIMPLE_SIMPLE_ENTRY_IMPL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "net/disk_cache/disk_cache.h"

namespace net {
class IOBuffer;
}

namespace disk_cache {

class SimpleSynchronousEntry;

// SimpleEntryImpl is the IO thread interface to an entry in the very simple
// disk cache. It proxies for the SimpleSynchronousEntry, which performs IO
// on the worker thread.
class SimpleEntryImpl : public Entry {
 public:
  static int OpenEntry(const base::FilePath& path,
                       const std::string& key,
                       Entry** entry,
                       const CompletionCallback& callback);

  static int CreateEntry(const base::FilePath& path,
                         const std::string& key,
                         Entry** entry,
                         const CompletionCallback& callback);

  static int DoomEntry(const base::FilePath& path,
                       const std::string& key,
                       const CompletionCallback& callback);

  // From Entry:
  virtual void Doom() OVERRIDE;
  virtual void Close() OVERRIDE;
  virtual std::string GetKey() const OVERRIDE;
  virtual base::Time GetLastUsed() const OVERRIDE;
  virtual base::Time GetLastModified() const OVERRIDE;
  virtual int32 GetDataSize(int index) const OVERRIDE;
  virtual int ReadData(int index,
                       int offset,
                       net::IOBuffer* buf,
                       int buf_len,
                       const CompletionCallback& callback) OVERRIDE;
  virtual int WriteData(int index,
                        int offset,
                        net::IOBuffer* buf,
                        int buf_len,
                        const CompletionCallback& callback,
                        bool truncate) OVERRIDE;
  virtual int ReadSparseData(int64 offset,
                             net::IOBuffer* buf,
                             int buf_len,
                             const CompletionCallback& callback) OVERRIDE;
  virtual int WriteSparseData(int64 offset,
                              net::IOBuffer* buf,
                              int buf_len,
                              const CompletionCallback& callback) OVERRIDE;
  virtual int GetAvailableRange(int64 offset,
                                int len,
                                int64* start,
                                const CompletionCallback& callback) OVERRIDE;
  virtual bool CouldBeSparse() const OVERRIDE;
  virtual void CancelSparseIO() OVERRIDE;
  virtual int ReadyForSparseIO(const CompletionCallback& callback) OVERRIDE;

 private:
  explicit SimpleEntryImpl(SimpleSynchronousEntry* synchronous_entry);

  virtual ~SimpleEntryImpl();

  // Called after a SimpleSynchronousEntry has completed CreateEntry() or
  // OpenEntry(). Constructs the new SimpleEntryImpl (if |result| is net::OK)
  // and passes it back to the caller via |out_entry|. Also runs
  // |completion_callback|.
  static void CreationOperationComplete(
      const CompletionCallback& completion_callback,
      Entry** out_entry,
      SimpleSynchronousEntry* sync_entry);

  // Called after a SimpleSynchronousEntry has completed an asynchronous IO
  // operation, such as ReadData() or WriteData(). Calls |completion_callback|.
  static void EntryOperationComplete(
      const CompletionCallback& completion_callback,
      base::WeakPtr<SimpleEntryImpl> entry,
      int result);

  base::WeakPtrFactory<SimpleEntryImpl> weak_ptr_factory_;
  std::string key_;

  // The |synchronous_entry_| is the worker thread object that performs IO on
  // entries.
  SimpleSynchronousEntry* synchronous_entry_;

  // Set to true when a worker operation is posted on the |synchronous_entry_|,
  // and false after. Used to insure thread safety by not allowing multiple
  // threads to access the |synchronous_entry_| simultaneously.
  bool synchronous_entry_in_use_by_worker_;

  bool has_been_doomed_;
};

}  // namespace disk_cache

#endif  // NET_DISK_CACHE_SIMPLE_SIMPLE_ENTRY_IMPL_H_
