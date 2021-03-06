// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/drive/sync_client.h"

#include <vector>

#include "base/bind.h"
#include "base/message_loop/message_loop_proxy.h"
#include "chrome/browser/chromeos/drive/drive.pb.h"
#include "chrome/browser/chromeos/drive/file_cache.h"
#include "chrome/browser/chromeos/drive/file_system/download_operation.h"
#include "chrome/browser/chromeos/drive/file_system/update_operation.h"
#include "chrome/browser/chromeos/drive/file_system_util.h"
#include "content/public/browser/browser_thread.h"

using content::BrowserThread;

namespace drive {
namespace internal {

namespace {

// The delay constant is used to delay processing a sync task. We should not
// process SyncTasks immediately for the following reasons:
//
// 1) For fetching, the user may accidentally click on "Make available
//    offline" checkbox on a file, and immediately cancel it in a second.
//    It's a waste to fetch the file in this case.
//
// 2) For uploading, file writing via HTML5 file system API is performed in
//    two steps: 1) truncate a file to 0 bytes, 2) write contents. We
//    shouldn't start uploading right after the step 1). Besides, the user
//    may edit the same file repeatedly in a short period of time.
//
// TODO(satorux): We should find a way to handle the upload case more nicely,
// and shorten the delay. crbug.com/134774
const int kDelaySeconds = 5;

// Appends |resource_id| to |to_fetch| if the file is pinned but not fetched
// (not present locally), or to |to_upload| if the file is dirty but not
// uploaded.
void CollectBacklog(std::vector<std::string>* to_fetch,
                    std::vector<std::string>* to_upload,
                    const std::string& resource_id,
                    const FileCacheEntry& cache_entry) {
  DCHECK(to_fetch);
  DCHECK(to_upload);

  if (cache_entry.is_pinned() && !cache_entry.is_present())
    to_fetch->push_back(resource_id);

  if (cache_entry.is_dirty())
    to_upload->push_back(resource_id);
}

}  // namespace

SyncClient::SyncClient(base::SequencedTaskRunner* blocking_task_runner,
                       file_system::OperationObserver* observer,
                       JobScheduler* scheduler,
                       ResourceMetadata* metadata,
                       FileCache* cache)
    : metadata_(metadata),
      cache_(cache),
      download_operation_(new file_system::DownloadOperation(
          blocking_task_runner,
          observer,
          scheduler,
          metadata,
          cache)),
      update_operation_(new file_system::UpdateOperation(blocking_task_runner,
                                                         observer,
                                                         scheduler,
                                                         metadata,
                                                         cache)),
      delay_(base::TimeDelta::FromSeconds(kDelaySeconds)),
      weak_ptr_factory_(this) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

SyncClient::~SyncClient() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

void SyncClient::StartProcessingBacklog() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  std::vector<std::string>* to_fetch = new std::vector<std::string>;
  std::vector<std::string>* to_upload = new std::vector<std::string>;
  cache_->IterateOnUIThread(base::Bind(&CollectBacklog, to_fetch, to_upload),
                            base::Bind(&SyncClient::OnGetResourceIdsOfBacklog,
                                       weak_ptr_factory_.GetWeakPtr(),
                                       base::Owned(to_fetch),
                                       base::Owned(to_upload)));
}

void SyncClient::StartCheckingExistingPinnedFiles() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  cache_->IterateOnUIThread(
      base::Bind(&SyncClient::OnGetResourceIdOfExistingPinnedFile,
                 weak_ptr_factory_.GetWeakPtr()),
      base::Bind(&base::DoNothing));
}

void SyncClient::AddFetchTask(const std::string& resource_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  AddTaskToQueue(FETCH, resource_id);
}

void SyncClient::RemoveFetchTask(const std::string& resource_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // TODO(kinaba): Cancel tasks in JobScheduler as well. crbug.com/248856
  pending_fetch_list_.erase(resource_id);
}

void SyncClient::AddUploadTask(const std::string& resource_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  AddTaskToQueue(UPLOAD, resource_id);
}

void SyncClient::AddTaskToQueue(SyncType type,
                                const std::string& resource_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // If the same task is already queued, ignore this task.
  switch (type) {
    case FETCH:
      if (fetch_list_.find(resource_id) == fetch_list_.end()) {
        fetch_list_.insert(resource_id);
        pending_fetch_list_.insert(resource_id);
      } else {
        return;
      }
      break;
    case UPLOAD:
      if (upload_list_.find(resource_id) == upload_list_.end()) {
        upload_list_.insert(resource_id);
      } else {
        return;
      }
      break;
  }

  base::MessageLoopProxy::current()->PostDelayedTask(
      FROM_HERE,
      base::Bind(&SyncClient::StartTask,
                 weak_ptr_factory_.GetWeakPtr(),
                 type,
                 resource_id),
      delay_);
}

void SyncClient::StartTask(SyncType type, const std::string& resource_id) {
  switch (type) {
    case FETCH:
      // Check if the resource has been removed from the start list.
      if (pending_fetch_list_.find(resource_id) != pending_fetch_list_.end()) {
        DVLOG(1) << "Fetching " << resource_id;
        pending_fetch_list_.erase(resource_id);

        download_operation_->EnsureFileDownloadedByResourceId(
            resource_id,
            ClientContext(BACKGROUND),
            GetFileContentInitializedCallback(),
            google_apis::GetContentCallback(),
            base::Bind(&SyncClient::OnFetchFileComplete,
                       weak_ptr_factory_.GetWeakPtr(),
                       resource_id));
      } else {
        // Cancel the task.
        fetch_list_.erase(resource_id);
      }
      break;
    case UPLOAD:
      DVLOG(1) << "Uploading " << resource_id;
      update_operation_->UpdateFileByResourceId(
          resource_id,
          ClientContext(BACKGROUND),
          base::Bind(&SyncClient::OnUploadFileComplete,
                     weak_ptr_factory_.GetWeakPtr(),
                     resource_id));
      break;
  }
}

void SyncClient::OnGetResourceIdsOfBacklog(
    const std::vector<std::string>* to_fetch,
    const std::vector<std::string>* to_upload) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // Give priority to upload tasks over fetch tasks, so that dirty files are
  // uploaded as soon as possible.
  for (size_t i = 0; i < to_upload->size(); ++i) {
    const std::string& resource_id = (*to_upload)[i];
    DVLOG(1) << "Queuing to upload: " << resource_id;
    AddTaskToQueue(UPLOAD, resource_id);
  }

  for (size_t i = 0; i < to_fetch->size(); ++i) {
    const std::string& resource_id = (*to_fetch)[i];
    DVLOG(1) << "Queuing to fetch: " << resource_id;
    AddTaskToQueue(FETCH, resource_id);
  }
}

void SyncClient::OnGetResourceIdOfExistingPinnedFile(
    const std::string& resource_id,
    const FileCacheEntry& cache_entry) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (cache_entry.is_pinned() && cache_entry.is_present()) {
    metadata_->GetResourceEntryByIdOnUIThread(
        resource_id,
        base::Bind(&SyncClient::OnGetResourceEntryById,
                   weak_ptr_factory_.GetWeakPtr(),
                   resource_id,
                   cache_entry));
  }
}

void SyncClient::OnGetResourceEntryById(
    const std::string& resource_id,
    const FileCacheEntry& cache_entry,
    FileError error,
    scoped_ptr<ResourceEntry> entry) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (entry.get() && !entry->has_file_specific_info())
    error = FILE_ERROR_NOT_FOUND;

  if (error != FILE_ERROR_OK) {
    LOG(WARNING) << "Entry not found: " << resource_id;
    return;
  }

  // If MD5s don't match, it indicates the local cache file is stale, unless
  // the file is dirty (the MD5 is "local"). We should never re-fetch the
  // file when we have a locally modified version.
  if (entry->file_specific_info().md5() != cache_entry.md5() &&
      !cache_entry.is_dirty()) {
    cache_->RemoveOnUIThread(resource_id,
                             base::Bind(&SyncClient::OnRemove,
                                        weak_ptr_factory_.GetWeakPtr(),
                                        resource_id));
  }
}

void SyncClient::OnRemove(const std::string& resource_id,
                          FileError error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (error != FILE_ERROR_OK) {
    LOG(WARNING) << "Failed to remove cache entry: " << resource_id;
    return;
  }

  // Before fetching, we should pin this file again, so that the fetched file
  // is downloaded properly to the persistent directory and marked pinned.
  cache_->PinOnUIThread(resource_id,
                        std::string(),
                        base::Bind(&SyncClient::OnPinned,
                                   weak_ptr_factory_.GetWeakPtr(),
                                   resource_id));
}

void SyncClient::OnPinned(const std::string& resource_id,
                          FileError error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (error != FILE_ERROR_OK) {
    LOG(WARNING) << "Failed to pin cache entry: " << resource_id;
    return;
  }

  // Finally, adding to the queue.
  AddTaskToQueue(FETCH, resource_id);
}

void SyncClient::OnFetchFileComplete(const std::string& resource_id,
                                     FileError error,
                                     const base::FilePath& local_path,
                                     scoped_ptr<ResourceEntry> entry) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  fetch_list_.erase(resource_id);

  if (error == FILE_ERROR_OK) {
    DVLOG(1) << "Fetched " << resource_id << ": "
             << local_path.value();
  } else {
    switch (error) {
      case FILE_ERROR_ABORT:
        // If user cancels download, unpin the file so that we do not sync the
        // file again.
        cache_->UnpinOnUIThread(resource_id,
                                std::string(),
                                base::Bind(&util::EmptyFileOperationCallback));
        break;
      case FILE_ERROR_NO_CONNECTION:
        // Re-queue the task so that we'll retry once the connection is back.
        AddTaskToQueue(FETCH, resource_id);
        break;
      default:
        LOG(WARNING) << "Failed to fetch " << resource_id
                     << ": " << FileErrorToString(error);
    }
  }
}

void SyncClient::OnUploadFileComplete(const std::string& resource_id,
                                      FileError error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  upload_list_.erase(resource_id);

  if (error == FILE_ERROR_OK) {
    DVLOG(1) << "Uploaded " << resource_id;
  } else {
    // TODO(satorux): We should re-queue if the error is recoverable.
    LOG(WARNING) << "Failed to upload " << resource_id << ": "
                 << FileErrorToString(error);
  }
}

}  // namespace internal
}  // namespace drive
