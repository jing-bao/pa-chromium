// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/google_apis/fake_drive_service.h"

#include <string>

#include "base/logging.h"
#include "base/message_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_tokenizer.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/google_apis/drive_api_parser.h"
#include "chrome/browser/google_apis/gdata_wapi_parser.h"
#include "chrome/browser/google_apis/test_util.h"
#include "chrome/browser/google_apis/time_util.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/escape.h"
#include "net/base/url_util.h"

using content::BrowserThread;

namespace google_apis {
namespace {

// Rel property of upload link in the entries dictionary value.
const char kUploadUrlRel[] =
    "http://schemas.google.com/g/2005#resumable-create-media";

// Returns true if a resource entry matches with the search query.
// Supports queries consist of following format.
// - Phrases quoted by double/single quotes
// - AND search for multiple words/phrases segmented by space
// - Limited attribute search.  Only "title:" is supported.
bool EntryMatchWithQuery(const ResourceEntry& entry,
                         const std::string& query) {
  base::StringTokenizer tokenizer(query, " ");
  tokenizer.set_quote_chars("\"'");
  while (tokenizer.GetNext()) {
    std::string key, value;
    const std::string& token = tokenizer.token();
    if (token.find(':') == std::string::npos) {
      TrimString(token, "\"'", &value);
    } else {
      base::StringTokenizer key_value(token, ":");
      key_value.set_quote_chars("\"'");
      if (!key_value.GetNext())
        return false;
      key = key_value.token();
      if (!key_value.GetNext())
        return false;
      TrimString(key_value.token(), "\"'", &value);
    }

    // TODO(peria): Deal with other attributes than title.
    if (!key.empty() && key != "title")
      return false;
    // Search query in the title.
    if (entry.title().find(value) == std::string::npos)
      return false;
  }
  return true;
}

// Gets the upload URL from the given entry. Returns an empty URL if not
// found.
GURL GetUploadUrl(const base::DictionaryValue& entry) {
  std::string upload_url;
  const base::ListValue* links = NULL;
  if (entry.GetList("link", &links) && links) {
    for (size_t link_index = 0;
         link_index < links->GetSize();
         ++link_index) {
      const base::DictionaryValue* link = NULL;
      std::string rel;
      if (links->GetDictionary(link_index, &link) &&
          link && link->GetString("rel", &rel) &&
          rel == kUploadUrlRel &&
          link->GetString("href", &upload_url)) {
        break;
      }
    }
  }
  return GURL(upload_url);
}

// Returns |url| without query parameter.
GURL RemoveQueryParameter(const GURL& url) {
  GURL::Replacements replacements;
  replacements.ClearQuery();
  return url.ReplaceComponents(replacements);
}

}  // namespace

FakeDriveService::FakeDriveService()
    : largest_changestamp_(0),
      default_max_results_(0),
      resource_id_count_(0),
      resource_list_load_count_(0),
      change_list_load_count_(0),
      directory_load_count_(0),
      about_resource_load_count_(0),
      offline_(false) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

FakeDriveService::~FakeDriveService() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

bool FakeDriveService::LoadResourceListForWapi(
    const std::string& relative_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  scoped_ptr<Value> raw_value = test_util::LoadJSONFile(relative_path);
  base::DictionaryValue* as_dict = NULL;
  base::Value* feed = NULL;
  base::DictionaryValue* feed_as_dict = NULL;

  // Extract the "feed" from the raw value and take the ownership.
  // Note that Remove() transfers the ownership to |feed|.
  if (raw_value->GetAsDictionary(&as_dict) &&
      as_dict->Remove("feed", &feed) &&
      feed->GetAsDictionary(&feed_as_dict)) {
    resource_list_value_.reset(feed_as_dict);

    // Go through entries and convert test$data from a string to a binary blob.
    base::ListValue* entries = NULL;
    if (feed_as_dict->GetList("entry", &entries)) {
      for (size_t i = 0; i < entries->GetSize(); ++i) {
        base::DictionaryValue* entry = NULL;
        std::string content_data;
        if (entries->GetDictionary(i, &entry) &&
            entry->GetString("test$data", &content_data)) {
          entry->Set("test$data", base::BinaryValue::CreateWithCopiedBuffer(
                  content_data.c_str(), content_data.size()));
        }
      }
    }
  }

  return resource_list_value_;
}

bool FakeDriveService::LoadAccountMetadataForWapi(
    const std::string& relative_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  account_metadata_value_ = test_util::LoadJSONFile(relative_path);

  // Update the largest_changestamp_.
  scoped_ptr<AccountMetadata> account_metadata =
      AccountMetadata::CreateFrom(*account_metadata_value_);
  largest_changestamp_ = account_metadata->largest_changestamp();

  // Add the largest changestamp to the existing entries.
  // This will be used to generate change lists in GetResourceList().
  if (resource_list_value_) {
    base::ListValue* entries = NULL;
    if (resource_list_value_->GetList("entry", &entries)) {
      for (size_t i = 0; i < entries->GetSize(); ++i) {
        base::DictionaryValue* entry = NULL;
        if (entries->GetDictionary(i, &entry)) {
          entry->SetString("docs$changestamp.value",
                           base::Int64ToString(largest_changestamp_));
        }
      }
    }
  }

  return account_metadata_value_;
}

bool FakeDriveService::LoadAppListForDriveApi(
    const std::string& relative_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  app_info_value_ = test_util::LoadJSONFile(relative_path);
  return app_info_value_;
}

GURL FakeDriveService::GetFakeLinkUrl(const std::string& resource_id) {
  return GURL("https://fake_server/" + net::EscapePath(resource_id));
}

void FakeDriveService::Initialize(Profile* profile) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

void FakeDriveService::AddObserver(DriveServiceObserver* observer) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

void FakeDriveService::RemoveObserver(DriveServiceObserver* observer) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

bool FakeDriveService::CanSendRequest() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return true;
}

void FakeDriveService::CancelAll() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

bool FakeDriveService::CancelForFilePath(const base::FilePath& file_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  last_cancelled_file_ = file_path;
  return true;
}

std::string FakeDriveService::CanonicalizeResourceId(
    const std::string& resource_id) const {
  return resource_id;
}

bool FakeDriveService::HasAccessToken() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return true;
}

bool FakeDriveService::HasRefreshToken() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return true;
}

void FakeDriveService::ClearAccessToken() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

void FakeDriveService::ClearRefreshToken() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
}

std::string FakeDriveService::GetRootResourceId() const {
  return "fake_root";
}

CancelCallback FakeDriveService::GetAllResourceList(
    const GetResourceListCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  GetResourceListInternal(0,  // start changestamp
                          std::string(),  // empty search query
                          std::string(),  // no directory resource id,
                          0,  // start offset
                          default_max_results_,
                          &resource_list_load_count_,
                          callback);
  return CancelCallback();
}

CancelCallback FakeDriveService::GetResourceListInDirectory(
    const std::string& directory_resource_id,
    const GetResourceListCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!directory_resource_id.empty());
  DCHECK(!callback.is_null());

  GetResourceListInternal(0,  // start changestamp
                          std::string(),  // empty search query
                          directory_resource_id,
                          0,  // start offset
                          default_max_results_,
                          &directory_load_count_,
                          callback);
  return CancelCallback();
}

CancelCallback FakeDriveService::Search(
    const std::string& search_query,
    const GetResourceListCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!search_query.empty());
  DCHECK(!callback.is_null());

  GetResourceListInternal(0,  // start changestamp
                          search_query,
                          std::string(),  // no directory resource id,
                          0,  // start offset
                          default_max_results_,
                          NULL,
                          callback);
  return CancelCallback();
}

CancelCallback FakeDriveService::SearchByTitle(
    const std::string& title,
    const std::string& directory_resource_id,
    const GetResourceListCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!title.empty());
  DCHECK(!callback.is_null());

  // Note: the search implementation here doesn't support quotation unescape,
  // so don't escape here.
  GetResourceListInternal(0,  // start changestamp
                          base::StringPrintf("title:'%s'", title.c_str()),
                          directory_resource_id,
                          0,  // start offset
                          default_max_results_,
                          NULL,
                          callback);
  return CancelCallback();
}

CancelCallback FakeDriveService::GetChangeList(
    int64 start_changestamp,
    const GetResourceListCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  GetResourceListInternal(start_changestamp,
                          std::string(),  // empty search query
                          std::string(),  // no directory resource id,
                          0,  // start offset
                          default_max_results_,
                          &change_list_load_count_,
                          callback);
  return CancelCallback();
}

CancelCallback FakeDriveService::ContinueGetResourceList(
    const GURL& override_url,
    const GetResourceListCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!override_url.is_empty());
  DCHECK(!callback.is_null());

  // "changestamp", "q", "parent" and "start-offset" are parameters to
  // implement "paging" of the result on FakeDriveService.
  // The URL should be the one filled in GetResourceListInternal of the
  // previous method invocation, so it should start with "http://localhost/?".
  // See also GetResourceListInternal.
  DCHECK_EQ(override_url.host(), "localhost");
  DCHECK_EQ(override_url.path(), "/");

  int64 start_changestamp = 0;
  std::string search_query;
  std::string directory_resource_id;
  int start_offset = 0;
  int max_results = default_max_results_;
  std::vector<std::pair<std::string, std::string> > parameters;
  if (base::SplitStringIntoKeyValuePairs(
          override_url.query(), '=', '&', &parameters)) {
    for (size_t i = 0; i < parameters.size(); ++i) {
      if (parameters[i].first == "changestamp") {
        base::StringToInt64(parameters[i].second, &start_changestamp);
      } else if (parameters[i].first == "q") {
        search_query =
            net::UnescapeURLComponent(parameters[i].second,
                                      net::UnescapeRule::URL_SPECIAL_CHARS);
      } else if (parameters[i].first == "parent") {
        directory_resource_id =
            net::UnescapeURLComponent(parameters[i].second,
                                      net::UnescapeRule::URL_SPECIAL_CHARS);
      } else if (parameters[i].first == "start-offset") {
        base::StringToInt(parameters[i].second, &start_offset);
      } else if (parameters[i].first == "max-results") {
        base::StringToInt(parameters[i].second, &max_results);
      }
    }
  }

  GetResourceListInternal(
      start_changestamp, search_query, directory_resource_id,
      start_offset, max_results, NULL, callback);
  return CancelCallback();
}

CancelCallback FakeDriveService::GetResourceEntry(
    const std::string& resource_id,
    const GetResourceEntryCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    scoped_ptr<ResourceEntry> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   GDATA_NO_CONNECTION,
                   base::Passed(&null)));
    return CancelCallback();
  }

  base::DictionaryValue* entry = FindEntryByResourceId(resource_id);
  if (entry) {
    scoped_ptr<ResourceEntry> resource_entry =
        ResourceEntry::CreateFrom(*entry);
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, HTTP_SUCCESS, base::Passed(&resource_entry)));
    return CancelCallback();
  }

  scoped_ptr<ResourceEntry> null;
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_NOT_FOUND, base::Passed(&null)));
  return CancelCallback();
}

CancelCallback FakeDriveService::GetAboutResource(
    const GetAboutResourceCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    scoped_ptr<AboutResource> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   GDATA_NO_CONNECTION, base::Passed(&null)));
    return CancelCallback();
  }

  ++about_resource_load_count_;
  scoped_ptr<AboutResource> about_resource(
      AboutResource::CreateFromAccountMetadata(
          *AccountMetadata::CreateFrom(*account_metadata_value_),
          GetRootResourceId()));
  // Overwrite the change id.
  about_resource->set_largest_change_id(largest_changestamp_);
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback,
                 HTTP_SUCCESS, base::Passed(&about_resource)));
  return CancelCallback();
}

CancelCallback FakeDriveService::GetAppList(
    const GetAppListCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());
  DCHECK(app_info_value_);

  if (offline_) {
    scoped_ptr<AppList> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   GDATA_NO_CONNECTION,
                   base::Passed(&null)));
    return CancelCallback();
  }

  scoped_ptr<AppList> app_list(AppList::CreateFrom(*app_info_value_));
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_SUCCESS, base::Passed(&app_list)));
  return CancelCallback();
}

CancelCallback FakeDriveService::DeleteResource(
    const std::string& resource_id,
    const std::string& etag,
    const EntryActionCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(callback, GDATA_NO_CONNECTION));
    return CancelCallback();
  }

  base::ListValue* entries = NULL;
  // Go through entries and remove the one that matches |resource_id|.
  if (resource_list_value_->GetList("entry", &entries)) {
    for (size_t i = 0; i < entries->GetSize(); ++i) {
      base::DictionaryValue* entry = NULL;
      std::string current_resource_id;
      if (entries->GetDictionary(i, &entry) &&
          entry->GetString("gd$resourceId.$t", &current_resource_id) &&
          resource_id == current_resource_id) {
        entries->Remove(i, NULL);
        base::MessageLoop::current()->PostTask(
            FROM_HERE, base::Bind(callback, HTTP_SUCCESS));
        return CancelCallback();
      }
    }
  }

  // TODO(satorux): Add support for returning "deleted" entries in
  // changelists from GetResourceList().
  base::MessageLoop::current()->PostTask(
      FROM_HERE, base::Bind(callback, HTTP_NOT_FOUND));
  return CancelCallback();
}

CancelCallback FakeDriveService::DownloadFile(
    const base::FilePath& virtual_path,
    const base::FilePath& local_cache_path,
    const GURL& download_url,
    const DownloadActionCallback& download_action_callback,
    const GetContentCallback& get_content_callback,
    const ProgressCallback& progress_callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!download_action_callback.is_null());

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(download_action_callback,
                   GDATA_NO_CONNECTION,
                   base::FilePath()));
    return CancelCallback();
  }

  // The field content.src is the URL to download the file.
  base::DictionaryValue* entry = FindEntryByContentUrl(download_url);
  if (!entry) {
    base::MessageLoopProxy::current()->PostTask(
        FROM_HERE,
        base::Bind(download_action_callback, HTTP_NOT_FOUND, base::FilePath()));
    return CancelCallback();
  }

  // Write "x"s of the file size specified in the entry.
  std::string file_size_string;
  entry->GetString("docs$size.$t", &file_size_string);
  // TODO(satorux): To be correct, we should update docs$md5Checksum.$t here.
  int64 file_size = 0;
  if (base::StringToInt64(file_size_string, &file_size)) {
    base::BinaryValue* content_binary_data;
    std::string content_data;
    if (entry->GetBinary("test$data", &content_binary_data)) {
      content_data = std::string(content_binary_data->GetBuffer(),
          content_binary_data->GetSize());
    }
    DCHECK_EQ(static_cast<size_t>(file_size), content_data.size());

    if (!get_content_callback.is_null()) {
      const int64 kBlockSize = 5;
      for (int64 i = 0; i < file_size; i += kBlockSize) {
        const int64 size = std::min(kBlockSize, file_size - i);
        scoped_ptr<std::string> content_for_callback(
            new std::string(content_data.substr(i, size)));
        base::MessageLoopProxy::current()->PostTask(
            FROM_HERE,
            base::Bind(get_content_callback, HTTP_SUCCESS,
                       base::Passed(&content_for_callback)));
      }
    }

    if (test_util::WriteStringToFile(local_cache_path, content_data)) {
      if (!progress_callback.is_null()) {
        // See also the comment in ResumeUpload(). For testing that clients
        // can handle the case progress_callback is called multiple times,
        // here we invoke the callback twice.
        base::MessageLoopProxy::current()->PostTask(
            FROM_HERE,
            base::Bind(progress_callback, file_size / 2, file_size));
        base::MessageLoopProxy::current()->PostTask(
            FROM_HERE,
            base::Bind(progress_callback, file_size, file_size));
      }
      base::MessageLoopProxy::current()->PostTask(
          FROM_HERE,
          base::Bind(download_action_callback,
                     HTTP_SUCCESS,
                     local_cache_path));
      return CancelCallback();
    }
  }

  // Failed to write the content.
  base::MessageLoopProxy::current()->PostTask(
      FROM_HERE,
      base::Bind(download_action_callback, GDATA_FILE_ERROR, base::FilePath()));
  return CancelCallback();
}

CancelCallback FakeDriveService::CopyResource(
    const std::string& resource_id,
    const std::string& in_parent_resource_id,
    const std::string& new_name,
    const GetResourceEntryCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    scoped_ptr<ResourceEntry> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   GDATA_NO_CONNECTION,
                   base::Passed(&null)));
    return CancelCallback();
  }

  const std::string& parent_resource_id = in_parent_resource_id.empty() ?
      GetRootResourceId() : in_parent_resource_id;

  base::ListValue* entries = NULL;
  // Go through entries and copy the one that matches |resource_id|.
  if (resource_list_value_->GetList("entry", &entries)) {
    for (size_t i = 0; i < entries->GetSize(); ++i) {
      base::DictionaryValue* entry = NULL;
      std::string current_resource_id;
      if (entries->GetDictionary(i, &entry) &&
          entry->GetString("gd$resourceId.$t", &current_resource_id) &&
          resource_id == current_resource_id) {
        // Make a copy and set the new resource ID and the new title.
        scoped_ptr<DictionaryValue> copied_entry(entry->DeepCopy());
        copied_entry->SetString("gd$resourceId.$t",
                                resource_id + "_copied");
        copied_entry->SetString("title.$t", new_name);

        // Reset parent directory.
        base::ListValue* links = NULL;
        if (!entry->GetList("link", &links)) {
          links = new base::ListValue;
          entry->Set("link", links);
        }
        links->Clear();

        base::DictionaryValue* link = new base::DictionaryValue;
        link->SetString(
            "rel", "http://schemas.google.com/docs/2007#parent");
        link->SetString("href", GetFakeLinkUrl(parent_resource_id).spec());
        links->Append(link);

        AddNewChangestamp(copied_entry.get());

        // Parse the new entry.
        scoped_ptr<ResourceEntry> resource_entry =
            ResourceEntry::CreateFrom(*copied_entry);
        // Add it to the resource list.
        entries->Append(copied_entry.release());

        base::MessageLoop::current()->PostTask(
            FROM_HERE,
            base::Bind(callback,
                       HTTP_SUCCESS,
                       base::Passed(&resource_entry)));
        return CancelCallback();
      }
    }
  }

  scoped_ptr<ResourceEntry> null;
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_NOT_FOUND, base::Passed(&null)));
  return CancelCallback();
}

CancelCallback FakeDriveService::CopyHostedDocument(
    const std::string& resource_id,
    const std::string& new_name,
    const GetResourceEntryCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  return CopyResource(resource_id, std::string(), new_name, callback);
}

CancelCallback FakeDriveService::RenameResource(
    const std::string& resource_id,
    const std::string& new_name,
    const EntryActionCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(callback, GDATA_NO_CONNECTION));
    return CancelCallback();
  }

  base::DictionaryValue* entry = FindEntryByResourceId(resource_id);
  if (entry) {
    entry->SetString("title.$t", new_name);
    AddNewChangestamp(entry);
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(callback, HTTP_SUCCESS));
    return CancelCallback();
  }

  base::MessageLoop::current()->PostTask(
      FROM_HERE, base::Bind(callback, HTTP_NOT_FOUND));
  return CancelCallback();
}

CancelCallback FakeDriveService::TouchResource(
    const std::string& resource_id,
    const base::Time& modified_date,
    const base::Time& last_viewed_by_me_date,
    const GetResourceEntryCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!modified_date.is_null());
  DCHECK(!last_viewed_by_me_date.is_null());
  DCHECK(!callback.is_null());

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, GDATA_NO_CONNECTION,
                   base::Passed(scoped_ptr<ResourceEntry>())));
    return CancelCallback();
  }

  base::DictionaryValue* entry = FindEntryByResourceId(resource_id);
  if (!entry) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, HTTP_NOT_FOUND,
                   base::Passed(scoped_ptr<ResourceEntry>())));
    return CancelCallback();
  }

  entry->SetString("updated.$t",
                   util::FormatTimeAsString(modified_date));
  entry->SetString("gd$lastViewed.$t",
                   util::FormatTimeAsString(last_viewed_by_me_date));
  AddNewChangestamp(entry);

  scoped_ptr<ResourceEntry> parsed_entry(ResourceEntry::CreateFrom(*entry));
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_SUCCESS, base::Passed(&parsed_entry)));
  return CancelCallback();
}

CancelCallback FakeDriveService::AddResourceToDirectory(
    const std::string& parent_resource_id,
    const std::string& resource_id,
    const EntryActionCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(callback, GDATA_NO_CONNECTION));
    return CancelCallback();
  }

  base::DictionaryValue* entry = FindEntryByResourceId(resource_id);
  if (entry) {
    base::ListValue* links = NULL;
    if (!entry->GetList("link", &links)) {
      links = new base::ListValue;
      entry->Set("link", links);
    }

    // On the real Drive server, resources do not necessary shape a tree
    // structure. That is, each resource can have multiple parent.
    // We mimic the behavior here; AddResourceToDirectoy just adds
    // one more parent link, not overwriting old links.
    base::DictionaryValue* link = new base::DictionaryValue;
    link->SetString("rel", "http://schemas.google.com/docs/2007#parent");
    link->SetString(
        "href", GetFakeLinkUrl(parent_resource_id).spec());
    links->Append(link);

    AddNewChangestamp(entry);
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(callback, HTTP_SUCCESS));
    return CancelCallback();
  }

  base::MessageLoop::current()->PostTask(
      FROM_HERE, base::Bind(callback, HTTP_NOT_FOUND));
  return CancelCallback();
}

CancelCallback FakeDriveService::RemoveResourceFromDirectory(
    const std::string& parent_resource_id,
    const std::string& resource_id,
    const EntryActionCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(callback, GDATA_NO_CONNECTION));
    return CancelCallback();
  }

  base::DictionaryValue* entry = FindEntryByResourceId(resource_id);
  if (entry) {
    base::ListValue* links = NULL;
    if (entry->GetList("link", &links)) {
      GURL parent_content_url = GetFakeLinkUrl(parent_resource_id);
      for (size_t i = 0; i < links->GetSize(); ++i) {
        base::DictionaryValue* link = NULL;
        std::string rel;
        std::string href;
        if (links->GetDictionary(i, &link) &&
            link->GetString("rel", &rel) &&
            link->GetString("href", &href) &&
            rel == "http://schemas.google.com/docs/2007#parent" &&
            GURL(href) == parent_content_url) {
          links->Remove(i, NULL);
          AddNewChangestamp(entry);
          base::MessageLoop::current()->PostTask(
              FROM_HERE, base::Bind(callback, HTTP_SUCCESS));
          return CancelCallback();
        }
      }
    }
  }

  base::MessageLoop::current()->PostTask(
      FROM_HERE, base::Bind(callback, HTTP_NOT_FOUND));
  return CancelCallback();
}

CancelCallback FakeDriveService::AddNewDirectory(
    const std::string& parent_resource_id,
    const std::string& directory_name,
    const GetResourceEntryCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    scoped_ptr<ResourceEntry> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   GDATA_NO_CONNECTION,
                   base::Passed(&null)));
    return CancelCallback();
  }

  const char kContentType[] = "application/atom+xml;type=feed";
  const base::DictionaryValue* new_entry = AddNewEntry(kContentType,
                                                       "",  // content_data
                                                       parent_resource_id,
                                                       directory_name,
                                                       false,  // shared_with_me
                                                       "folder");
  if (!new_entry) {
    scoped_ptr<ResourceEntry> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, HTTP_NOT_FOUND, base::Passed(&null)));
    return CancelCallback();
  }

  scoped_ptr<ResourceEntry> parsed_entry(ResourceEntry::CreateFrom(*new_entry));
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_CREATED, base::Passed(&parsed_entry)));
  return CancelCallback();
}

CancelCallback FakeDriveService::InitiateUploadNewFile(
    const base::FilePath& drive_file_path,
    const std::string& content_type,
    int64 content_length,
    const std::string& parent_resource_id,
    const std::string& title,
    const InitiateUploadCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, GDATA_NO_CONNECTION, GURL()));
    return CancelCallback();
  }

  // Content length should be zero, as we'll create an empty file first. The
  // content will be added in ResumeUpload().
  const base::DictionaryValue* new_entry = AddNewEntry(content_type,
                                                       "",  // content_data
                                                       parent_resource_id,
                                                       title,
                                                       false,  // shared_with_me
                                                       "file");
  if (!new_entry) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, HTTP_NOT_FOUND, GURL()));
    return CancelCallback();
  }
  const GURL upload_url = GetUploadUrl(*new_entry);
  DCHECK(upload_url.is_valid());

  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_SUCCESS,
                 net::AppendQueryParameter(upload_url, "mode", "newfile")));
  return CancelCallback();
}

CancelCallback FakeDriveService::InitiateUploadExistingFile(
    const base::FilePath& drive_file_path,
    const std::string& content_type,
    int64 content_length,
    const std::string& resource_id,
    const std::string& etag,
    const InitiateUploadCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, GDATA_NO_CONNECTION, GURL()));
    return CancelCallback();
  }

  DictionaryValue* entry = FindEntryByResourceId(resource_id);
  if (!entry) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, HTTP_NOT_FOUND, GURL()));
    return CancelCallback();
  }

  std::string entry_etag;
  entry->GetString("gd$etag", &entry_etag);
  if (!etag.empty() && etag != entry_etag) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, HTTP_PRECONDITION, GURL()));
    return CancelCallback();
  }
  entry->SetString("docs$size.$t", "0");

  const GURL upload_url = GetUploadUrl(*entry);
  DCHECK(upload_url.is_valid());

  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_SUCCESS,
                 net::AppendQueryParameter(upload_url, "mode", "existing")));
  return CancelCallback();
}

CancelCallback FakeDriveService::GetUploadStatus(
    const base::FilePath& drive_file_path,
    const GURL& upload_url,
    int64 content_length,
    const UploadRangeCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());
  return CancelCallback();
}

CancelCallback FakeDriveService::ResumeUpload(
      const base::FilePath& drive_file_path,
      const GURL& upload_url,
      int64 start_position,
      int64 end_position,
      int64 content_length,
      const std::string& content_type,
      const base::FilePath& local_file_path,
      const UploadRangeCallback& callback,
      const ProgressCallback& progress_callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  scoped_ptr<ResourceEntry> result_entry;

  if (offline_) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   UploadRangeResponse(GDATA_NO_CONNECTION,
                                       start_position,
                                       end_position),
                   base::Passed(&result_entry)));
    return CancelCallback();
  }

  DictionaryValue* entry = NULL;
  entry = FindEntryByUploadUrl(RemoveQueryParameter(upload_url));
  if (!entry) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   UploadRangeResponse(HTTP_NOT_FOUND,
                                       start_position,
                                       end_position),
                   base::Passed(&result_entry)));
    return CancelCallback();
  }

  // Chunks are required to be sent in such a ways that they fill from the start
  // of the not-yet-uploaded part with no gaps nor overlaps.
  std::string current_size_string;
  int64 current_size;
  if (!entry->GetString("docs$size.$t", &current_size_string) ||
      !base::StringToInt64(current_size_string, &current_size) ||
      current_size != start_position) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   UploadRangeResponse(HTTP_BAD_REQUEST,
                                       start_position,
                                       end_position),
                   base::Passed(&result_entry)));
    return CancelCallback();
  }

  entry->SetString("docs$size.$t", base::Int64ToString(end_position));

  if (!progress_callback.is_null()) {
    // In the real GDataWapi/Drive DriveService, progress is reported in
    // nondeterministic timing. In this fake implementation, we choose to call
    // it twice per one ResumeUpload. This is for making sure that client code
    // works fine even if the callback is invoked more than once; it is the
    // crucial difference of the progress callback from others.
    // Note that progress is notified in the relative offset in each chunk.
    const int64 chunk_size = end_position - start_position;
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(progress_callback, chunk_size / 2, chunk_size));
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(progress_callback, chunk_size, chunk_size));
  }

  if (content_length != end_position) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   UploadRangeResponse(HTTP_RESUME_INCOMPLETE,
                                       start_position,
                                       end_position),
                    base::Passed(&result_entry)));
    return CancelCallback();
  }

  AddNewChangestamp(entry);
  result_entry = ResourceEntry::CreateFrom(*entry).Pass();

  std::string upload_mode;
  bool upload_mode_found =
      net::GetValueForKeyInQuery(upload_url, "mode", &upload_mode);
  DCHECK(upload_mode_found &&
         (upload_mode == "newfile" || upload_mode == "existing"));

  GDataErrorCode return_code =
      upload_mode == "newfile" ? HTTP_CREATED : HTTP_SUCCESS;

  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback,
                 UploadRangeResponse(return_code,
                                     start_position,
                                     end_position),
                 base::Passed(&result_entry)));
  return CancelCallback();
}

CancelCallback FakeDriveService::AuthorizeApp(
    const std::string& resource_id,
    const std::string& app_id,
    const AuthorizeAppCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());
  return CancelCallback();
}

void FakeDriveService::AddNewFile(const std::string& content_type,
                                  const std::string& content_data,
                                  const std::string& parent_resource_id,
                                  const std::string& title,
                                  bool shared_with_me,
                                  const GetResourceEntryCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    scoped_ptr<ResourceEntry> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   GDATA_NO_CONNECTION,
                   base::Passed(&null)));
    return;
  }

  // Prepare "kind" for hosted documents. This only supports Google Document.
  std::string entry_kind;
  if (content_type == "application/vnd.google-apps.document")
    entry_kind = "document";
  else
    entry_kind = "file";

  const base::DictionaryValue* new_entry = AddNewEntry(content_type,
                                                       content_data,
                                                       parent_resource_id,
                                                       title,
                                                       shared_with_me,
                                                       entry_kind);
  if (!new_entry) {
    scoped_ptr<ResourceEntry> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, HTTP_NOT_FOUND, base::Passed(&null)));
    return;
  }

  scoped_ptr<ResourceEntry> parsed_entry(
      ResourceEntry::CreateFrom(*new_entry));
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_CREATED, base::Passed(&parsed_entry)));
}

void FakeDriveService::SetLastModifiedTime(
    const std::string& resource_id,
    const base::Time& last_modified_time,
    const GetResourceEntryCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!callback.is_null());

  if (offline_) {
    scoped_ptr<ResourceEntry> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   GDATA_NO_CONNECTION,
                   base::Passed(&null)));
    return;
  }

  base::DictionaryValue* entry = FindEntryByResourceId(resource_id);
  if (!entry) {
    scoped_ptr<ResourceEntry> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, HTTP_NOT_FOUND, base::Passed(&null)));
    return;
  }

  if (last_modified_time.is_null()) {
    entry->Remove("updated.$t", NULL);
  } else {
    entry->SetString("updated.$t",
                     util::FormatTimeAsString(last_modified_time));
  }

  scoped_ptr<ResourceEntry> parsed_entry(
      ResourceEntry::CreateFrom(*entry));
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, HTTP_SUCCESS, base::Passed(&parsed_entry)));
}

base::DictionaryValue* FakeDriveService::FindEntryByResourceId(
    const std::string& resource_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  base::ListValue* entries = NULL;
  // Go through entries and return the one that matches |resource_id|.
  if (resource_list_value_->GetList("entry", &entries)) {
    for (size_t i = 0; i < entries->GetSize(); ++i) {
      base::DictionaryValue* entry = NULL;
      std::string current_resource_id;
      if (entries->GetDictionary(i, &entry) &&
          entry->GetString("gd$resourceId.$t", &current_resource_id) &&
          resource_id == current_resource_id) {
        return entry;
      }
    }
  }

  return NULL;
}

base::DictionaryValue* FakeDriveService::FindEntryByContentUrl(
    const GURL& content_url) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  base::ListValue* entries = NULL;
  // Go through entries and return the one that matches |content_url|.
  if (resource_list_value_->GetList("entry", &entries)) {
    for (size_t i = 0; i < entries->GetSize(); ++i) {
      base::DictionaryValue* entry = NULL;
      std::string current_content_url;
      if (entries->GetDictionary(i, &entry) &&
          entry->GetString("content.src", &current_content_url) &&
          content_url == GURL(current_content_url)) {
        return entry;
      }
    }
  }

  return NULL;
}

base::DictionaryValue* FakeDriveService::FindEntryByUploadUrl(
    const GURL& upload_url) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  base::ListValue* entries = NULL;
  // Go through entries and return the one that matches |upload_url|.
  if (resource_list_value_->GetList("entry", &entries)) {
    for (size_t i = 0; i < entries->GetSize(); ++i) {
      base::DictionaryValue* entry = NULL;
      base::ListValue* links = NULL;
      if (entries->GetDictionary(i, &entry) &&
          entry->GetList("link", &links) &&
          links) {
        for (size_t link_index = 0;
            link_index < links->GetSize();
            ++link_index) {
          base::DictionaryValue* link = NULL;
          std::string rel;
          std::string found_upload_url;
          if (links->GetDictionary(link_index, &link) &&
              link && link->GetString("rel", &rel) &&
              rel == kUploadUrlRel &&
              link->GetString("href", &found_upload_url) &&
              GURL(found_upload_url) == upload_url) {
            return entry;
          }
        }
      }
    }
  }

  return NULL;
}

std::string FakeDriveService::GetNewResourceId() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  ++resource_id_count_;
  return base::StringPrintf("resource_id_%d", resource_id_count_);
}

void FakeDriveService::AddNewChangestamp(base::DictionaryValue* entry) {
  ++largest_changestamp_;
  entry->SetString("docs$changestamp.value",
                   base::Int64ToString(largest_changestamp_));
}

const base::DictionaryValue* FakeDriveService::AddNewEntry(
    const std::string& content_type,
    const std::string& content_data,
    const std::string& parent_resource_id,
    const std::string& title,
    bool shared_with_me,
    const std::string& entry_kind) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (parent_resource_id != GetRootResourceId() &&
      !FindEntryByResourceId(parent_resource_id)) {
    return NULL;
  }

  std::string resource_id = GetNewResourceId();
  GURL upload_url = GURL("https://xxx/upload/" + resource_id);

  scoped_ptr<base::DictionaryValue> new_entry(new base::DictionaryValue);
  // Set the resource ID and the title
  new_entry->SetString("gd$resourceId.$t", resource_id);
  new_entry->SetString("title.$t", title);
  new_entry->SetString("docs$filename", title);
  // Set the contents, size and MD5 for a file.
  if (entry_kind == "file") {
    new_entry->Set("test$data",
        base::BinaryValue::CreateWithCopiedBuffer(
            content_data.c_str(), content_data.size()));
    new_entry->SetString("docs$size.$t",
                         base::Int64ToString(content_data.size()));
    // TODO(satorux): Set the correct MD5 here.
    new_entry->SetString("docs$md5Checksum.$t",
                         "3b4385ebefec6e743574c76bbd0575de");
  }

  // Add "category" which sets the resource type to |entry_kind|.
  base::ListValue* categories = new base::ListValue;
  base::DictionaryValue* category = new base::DictionaryValue;
  category->SetString("scheme", "http://schemas.google.com/g/2005#kind");
  category->SetString("term", "http://schemas.google.com/docs/2007#" +
                      entry_kind);
  categories->Append(category);
  new_entry->Set("category", categories);
  if (shared_with_me) {
    base::DictionaryValue* shared_with_me_label = new base::DictionaryValue;
    shared_with_me_label->SetString("label", "shared-with-me");
    shared_with_me_label->SetString("scheme",
                                    "http://schemas.google.com/g/2005/labels");
    shared_with_me_label->SetString(
        "term", "http://schemas.google.com/g/2005/labels#shared");
    categories->Append(shared_with_me_label);
  }

  std::string escaped_resource_id = net::EscapePath(resource_id);

  // Add "content" which sets the content URL.
  base::DictionaryValue* content = new base::DictionaryValue;
  content->SetString("src", "https://xxx/content/" + escaped_resource_id);
  content->SetString("type", content_type);
  new_entry->Set("content", content);

  // Add "link" which sets the parent URL, the edit URL and the upload URL.
  base::ListValue* links = new base::ListValue;

  base::DictionaryValue* parent_link = new base::DictionaryValue;
  parent_link->SetString("href", GetFakeLinkUrl(parent_resource_id).spec());
  parent_link->SetString("rel",
                         "http://schemas.google.com/docs/2007#parent");
  links->Append(parent_link);

  base::DictionaryValue* edit_link = new base::DictionaryValue;
  edit_link->SetString("href", "https://xxx/edit/" + escaped_resource_id);
  edit_link->SetString("rel", "edit");
  links->Append(edit_link);

  base::DictionaryValue* upload_link = new base::DictionaryValue;
  upload_link->SetString("href", upload_url.spec());
  upload_link->SetString("rel", kUploadUrlRel);
  links->Append(upload_link);
  new_entry->Set("link", links);

  AddNewChangestamp(new_entry.get());

  // If there are no entries, prepare an empty entry to add.
  if (!resource_list_value_->HasKey("entry"))
    resource_list_value_->Set("entry", new ListValue);

  base::DictionaryValue* raw_new_entry = new_entry.release();
  base::ListValue* entries = NULL;
  if (resource_list_value_->GetList("entry", &entries))
    entries->Append(raw_new_entry);

  return raw_new_entry;
}

void FakeDriveService::GetResourceListInternal(
    int64 start_changestamp,
    const std::string& search_query,
    const std::string& directory_resource_id,
    int start_offset,
    int max_results,
    int* load_counter,
    const GetResourceListCallback& callback) {
  if (offline_) {
    scoped_ptr<ResourceList> null;
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(callback,
                   GDATA_NO_CONNECTION,
                   base::Passed(&null)));
    return;
  }

  scoped_ptr<ResourceList> resource_list =
      ResourceList::CreateFrom(*resource_list_value_);

  // Filter out entries per parameters like |directory_resource_id| and
  // |search_query|.
  ScopedVector<ResourceEntry>* entries = resource_list->mutable_entries();

  int num_entries_matched = 0;
  for (size_t i = 0; i < entries->size();) {
    ResourceEntry* entry = (*entries)[i];
    bool should_exclude = false;

    // If |directory_resource_id| is set, exclude the entry if it's not in
    // the target directory.
    if (!directory_resource_id.empty()) {
      // Get the parent resource ID of the entry.
      std::string parent_resource_id;
      const google_apis::Link* parent_link =
          entry->GetLinkByType(Link::LINK_PARENT);
      if (parent_link) {
        parent_resource_id =
            net::UnescapeURLComponent(parent_link->href().ExtractFileName(),
                                      net::UnescapeRule::URL_SPECIAL_CHARS);
      }
      if (directory_resource_id != parent_resource_id)
        should_exclude = true;
    }

    // If |search_query| is set, exclude the entry if it does not contain the
    // search query in the title.
    if (!should_exclude && !search_query.empty() &&
        !EntryMatchWithQuery(*entry, search_query)) {
      should_exclude = true;
    }

    // If |start_changestamp| is set, exclude the entry if the
    // changestamp is older than |largest_changestamp|.
    // See https://developers.google.com/google-apps/documents-list/
    // #retrieving_all_changes_since_a_given_changestamp
    if (start_changestamp > 0 && entry->changestamp() < start_changestamp)
      should_exclude = true;

    // The entry matched the criteria for inclusion.
    if (!should_exclude)
      ++num_entries_matched;

    // If |start_offset| is set, exclude the entry if the entry is before the
    // start index. <= instead of < as |num_entries_matched| was
    // already incremented.
    if (start_offset > 0 && num_entries_matched <= start_offset)
      should_exclude = true;

    if (should_exclude)
      entries->erase(entries->begin() + i);
    else
      ++i;
  }

  // If |max_results| is set, trim the entries if the number exceeded the max
  // results.
  if (max_results > 0 && entries->size() > static_cast<size_t>(max_results)) {
    entries->erase(entries->begin() + max_results, entries->end());
    // Adds the next URL.
    // Here, we embed information which is needed for continuing the
    // GetResourceList request in the next invocation into url query
    // parameters.
    GURL next_url(base::StringPrintf(
        "http://localhost/?start-offset=%d&max-results=%d",
        start_offset + max_results,
        max_results));
    if (start_changestamp > 0) {
      next_url = net::AppendOrReplaceQueryParameter(
          next_url, "changestamp",
          base::Int64ToString(start_changestamp).c_str());
    }
    if (!search_query.empty()) {
      next_url = net::AppendOrReplaceQueryParameter(
          next_url, "q", search_query);
    }
    if (!directory_resource_id.empty()) {
      next_url = net::AppendOrReplaceQueryParameter(
          next_url, "parent", directory_resource_id);
    }

    Link* link = new Link;
    link->set_type(Link::LINK_NEXT);
    link->set_href(next_url);
    resource_list->mutable_links()->push_back(link);
  }

  if (load_counter)
    *load_counter += 1;
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(callback,
                 HTTP_SUCCESS,
                 base::Passed(&resource_list)));
}

}  // namespace google_apis
