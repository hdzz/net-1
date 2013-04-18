// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/disk_cache/simple/simple_synchronous_entry.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/file_util.h"
#include "base/hash.h"
#include "base/location.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop_proxy.h"
#include "base/sha1.h"
#include "base/stringprintf.h"
#include "base/task_runner.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/disk_cache/simple/simple_util.h"

using base::ClosePlatformFile;
using base::FilePath;
using base::GetPlatformFileInfo;
using base::PlatformFileError;
using base::PlatformFileInfo;
using base::PLATFORM_FILE_CREATE;
using base::PLATFORM_FILE_OK;
using base::PLATFORM_FILE_OPEN;
using base::PLATFORM_FILE_READ;
using base::PLATFORM_FILE_WRITE;
using base::ReadPlatformFile;
using base::SingleThreadTaskRunner;
using base::Time;
using base::TruncatePlatformFile;
using base::WritePlatformFile;

namespace disk_cache {

using simple_util::ConvertEntryHashKeyToHexString;
using simple_util::GetEntryHashKeyAsHexString;
using simple_util::GetFilenameFromHexStringAndIndex;
using simple_util::GetFilenameFromKeyAndIndex;
using simple_util::GetDataSizeFromKeyAndFileSize;
using simple_util::GetFileSizeFromKeyAndDataSize;
using simple_util::GetFileOffsetFromKeyAndDataOffset;

// static
void SimpleSynchronousEntry::OpenEntry(
    const FilePath& path,
    const std::string& key,
    SingleThreadTaskRunner* callback_runner,
    const SynchronousCreationCallback& callback) {
  SimpleSynchronousEntry* sync_entry =
      new SimpleSynchronousEntry(callback_runner, path, key);
  int rv = sync_entry->InitializeForOpen();
  if (rv != net::OK) {
    sync_entry->Doom();
    delete sync_entry;
    sync_entry = NULL;
  }
  callback_runner->PostTask(FROM_HERE, base::Bind(callback, sync_entry));
}

// static
void SimpleSynchronousEntry::CreateEntry(
    const FilePath& path,
    const std::string& key,
    SingleThreadTaskRunner* callback_runner,
    const SynchronousCreationCallback& callback) {
  SimpleSynchronousEntry* sync_entry =
      new SimpleSynchronousEntry(callback_runner, path, key);
  int rv = sync_entry->InitializeForCreate();
  if (rv != net::OK) {
    if (rv != net::ERR_FILE_EXISTS) {
      sync_entry->Doom();
    }
    delete sync_entry;
    sync_entry = NULL;
  }
  callback_runner->PostTask(FROM_HERE, base::Bind(callback, sync_entry));
}

// static
bool SimpleSynchronousEntry::DeleteFilesForEntry(const FilePath& path,
                                                 const std::string& hash_key) {
  bool result = true;
  for (int i = 0; i < kSimpleEntryFileCount; ++i) {
    FilePath to_delete = path.AppendASCII(
        GetFilenameFromHexStringAndIndex(hash_key, i));
    if (!file_util::Delete(to_delete, false)) {
      result = false;
      DLOG(ERROR) << "Could not delete " << to_delete.MaybeAsASCII();
    }
  }
  return result;
}

// static
void SimpleSynchronousEntry::DoomEntry(
    const FilePath& path,
    const std::string& key,
    SingleThreadTaskRunner* callback_runner,
    const net::CompletionCallback& callback) {
  bool deleted_well = DeleteFilesForEntry(path,
                                          GetEntryHashKeyAsHexString(key));
  int result = deleted_well ? net::OK : net::ERR_FAILED;
  if (!callback.is_null())
    callback_runner->PostTask(FROM_HERE, base::Bind(callback, result));
}

// static
void SimpleSynchronousEntry::DoomEntrySet(
    scoped_ptr<std::vector<uint64> > key_hashes,
    const FilePath& path,
    SingleThreadTaskRunner* callback_runner,
    const net::CompletionCallback& callback) {
  bool deleted_well = true;
  for (std::vector<uint64>::const_iterator it = key_hashes->begin(),
       end = key_hashes->end(); it != end; ++it)
    deleted_well &= DeleteFilesForEntry(path,
                                        ConvertEntryHashKeyToHexString((*it)));
  int result = deleted_well ? net::OK : net::ERR_FAILED;
  if (!callback.is_null())
    callback_runner->PostTask(FROM_HERE, base::Bind(callback, result));
}

void SimpleSynchronousEntry::Close() {
  for (int i = 0; i < kSimpleEntryFileCount; ++i) {
    bool ALLOW_UNUSED result = ClosePlatformFile(files_[i]);
    DLOG_IF(INFO, !result) << "Could not Close() file.";
  }
  delete this;
}

void SimpleSynchronousEntry::ReadData(
    int index,
    int offset,
    net::IOBuffer* buf,
    int buf_len,
    const SynchronousOperationCallback& callback) {
  DCHECK(initialized_);

  int64 file_offset = GetFileOffsetFromKeyAndDataOffset(key_, offset);
  int bytes_read = ReadPlatformFile(files_[index], file_offset,
                                    buf->data(), buf_len);
  if (bytes_read > 0)
    last_used_ = Time::Now();
  int result = (bytes_read >= 0) ? bytes_read : net::ERR_FAILED;
  if (result == net::ERR_FAILED)
    Doom();
  callback_runner_->PostTask(FROM_HERE, base::Bind(callback, result));
}

void SimpleSynchronousEntry::WriteData(
    int index,
    int offset,
    net::IOBuffer* buf,
    int buf_len,
    const SynchronousOperationCallback& callback,
    bool truncate) {
  DCHECK(initialized_);

  int64 file_offset = GetFileOffsetFromKeyAndDataOffset(key_, offset);
  if (buf_len > 0) {
    if (WritePlatformFile(files_[index], file_offset, buf->data(), buf_len) !=
        buf_len) {
      Doom();
      callback_runner_->PostTask(FROM_HERE,
                                 base::Bind(callback, net::ERR_FAILED));
      return;
    }
    data_size_[index] = std::max(data_size_[index], offset + buf_len);
  }
  if (truncate) {
    data_size_[index] = offset + buf_len;
    if (!TruncatePlatformFile(files_[index], file_offset + buf_len)) {
      Doom();
      callback_runner_->PostTask(FROM_HERE,
                                 base::Bind(callback, net::ERR_FAILED));
      return;
    }
  }
  last_modified_ = Time::Now();
  callback_runner_->PostTask(FROM_HERE, base::Bind(callback, buf_len));
}

SimpleSynchronousEntry::SimpleSynchronousEntry(
    SingleThreadTaskRunner* callback_runner,
    const FilePath& path,
    const std::string& key)
    : callback_runner_(callback_runner),
      path_(path),
      key_(key),
      initialized_(false) {
}

SimpleSynchronousEntry::~SimpleSynchronousEntry() {
}

bool SimpleSynchronousEntry::OpenOrCreateFiles(bool create) {
  for (int i = 0; i < kSimpleEntryFileCount; ++i) {
    FilePath filename = path_.AppendASCII(GetFilenameFromKeyAndIndex(key_, i));
    int flags = PLATFORM_FILE_READ | PLATFORM_FILE_WRITE;
    if (create)
      flags |= PLATFORM_FILE_CREATE;
    else
      flags |= PLATFORM_FILE_OPEN;
    PlatformFileError error;
    files_[i] = CreatePlatformFile(filename, flags, NULL, &error);
    if (error != PLATFORM_FILE_OK) {
      DVLOG(8) << "CreatePlatformFile error " << error << " while "
               << (create ? "creating " : "opening ")
               << filename.MaybeAsASCII();
      while (--i >= 0) {
        bool ALLOW_UNUSED did_close = ClosePlatformFile(files_[i]);
        DLOG_IF(INFO, !did_close) << "Could not close file "
                                  << filename.MaybeAsASCII();
      }
      return false;
    }
  }

  for (int i = 0; i < kSimpleEntryFileCount; ++i) {
    PlatformFileInfo file_info;
    bool success = GetPlatformFileInfo(files_[i], &file_info);
    if (!success) {
      DLOG(WARNING) << "Could not get platform file info.";
      continue;
    }
    last_used_ = std::max(last_used_, file_info.last_accessed);
    last_modified_ = std::max(last_modified_, file_info.last_modified);
    data_size_[i] = GetDataSizeFromKeyAndFileSize(key_, file_info.size);
  }

  return true;
}

int64 SimpleSynchronousEntry::GetFileSize() const {
  int64 file_size = 0;
  for (int i = 0; i < kSimpleEntryFileCount; ++i) {
    file_size += GetFileSizeFromKeyAndDataSize(key_, data_size_[i]);
  }
  return file_size;
}

int SimpleSynchronousEntry::InitializeForOpen() {
  DCHECK(!initialized_);
  if (!OpenOrCreateFiles(false))
    return net::ERR_FAILED;

  for (int i = 0; i < kSimpleEntryFileCount; ++i) {
    SimpleFileHeader header;
    int header_read_result =
        ReadPlatformFile(files_[i], 0, reinterpret_cast<char*>(&header),
                         sizeof(header));
    if (header_read_result != sizeof(header)) {
      DLOG(WARNING) << "Cannot read header from entry.";
      return net::ERR_FAILED;
    }

    if (header.initial_magic_number != kSimpleInitialMagicNumber) {
      // TODO(gavinp): This seems very bad; for now we log at WARNING, but we
      // should give consideration to not saturating the log with these if that
      // becomes a problem.
      DLOG(WARNING) << "Magic number did not match.";
      return net::ERR_FAILED;
    }

    if (header.version != kSimpleVersion) {
      DLOG(WARNING) << "Unreadable version.";
      return net::ERR_FAILED;
    }

    scoped_ptr<char[]> key(new char[header.key_length]);
    int key_read_result = ReadPlatformFile(files_[i], sizeof(header),
                                           key.get(), header.key_length);
    if (key_read_result != implicit_cast<int>(header.key_length)) {
      DLOG(WARNING) << "Cannot read key from entry.";
      return net::ERR_FAILED;
    }
    if (header.key_length != key_.size() ||
        std::memcmp(key_.data(), key.get(), key_.size()) != 0) {
      // TODO(gavinp): Since the way we use Entry SHA to name entries means this
      // is expected to occur at some frequency, add unit_tests that this does
      // is handled gracefully at higher levels.
      DLOG(WARNING) << "Key mismatch on open.";
      return net::ERR_FAILED;
    }

    if (base::Hash(key.get(), header.key_length) != header.key_hash) {
      DLOG(WARNING) << "Hash mismatch on key.";
      return net::ERR_FAILED;
    }
  }

  initialized_ = true;
  return net::OK;
}

int SimpleSynchronousEntry::InitializeForCreate() {
  DCHECK(!initialized_);
  if (!OpenOrCreateFiles(true)) {
    DLOG(WARNING) << "Could not create platform files.";
    return net::ERR_FILE_EXISTS;
  }
  for (int i = 0; i < kSimpleEntryFileCount; ++i) {
    SimpleFileHeader header;
    header.initial_magic_number = kSimpleInitialMagicNumber;
    header.version = kSimpleVersion;

    header.key_length = key_.size();
    header.key_hash = base::Hash(key_);

    if (WritePlatformFile(files_[i], 0, reinterpret_cast<char*>(&header),
                          sizeof(header)) != sizeof(header)) {
      DLOG(WARNING) << "Could not write headers to new cache entry.";
      return net::ERR_FAILED;
    }

    if (WritePlatformFile(files_[i], sizeof(header), key_.data(),
                          key_.size()) != implicit_cast<int>(key_.size())) {
      DLOG(WARNING) << "Could not write keys to new cache entry.";
      return net::ERR_FAILED;
    }
  }
  initialized_ = true;
  return net::OK;
}

void SimpleSynchronousEntry::Doom() {
  // TODO(gavinp): Consider if we should guard against redundant Doom() calls.
  DeleteFilesForEntry(path_, GetEntryHashKeyAsHexString(key_));
}

}  // namespace disk_cache
