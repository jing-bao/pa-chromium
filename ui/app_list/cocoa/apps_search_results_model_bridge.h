// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_APP_LIST_COCOA_APPS_SEARCH_RESULTS_MODEL_BRIDGE_H_
#define UI_APP_LIST_COCOA_APPS_SEARCH_RESULTS_MODEL_BRIDGE_H_

#include "base/memory/scoped_nsobject.h"
#include "base/memory/scoped_vector.h"
#include "ui/app_list/app_list_model.h"
#include "ui/base/models/list_model_observer.h"

@class NSTableView;

namespace app_list {

// Bridge observing the ListModel representing search results in the app list,
// and updating the NSTableView where they are displayed.
class AppsSearchResultsModelBridge : public ui::ListModelObserver {
 public:
  AppsSearchResultsModelBridge(AppListModel::SearchResults* results_model,
                               NSTableView* results_table_view);
  virtual ~AppsSearchResultsModelBridge();

 private:
  // Lightweight observer to react to icon updates on individual results.
  class ItemObserver;

  void UpdateItemObservers();
  void ReloadDataForItems(size_t start, size_t count) const;

  // Overridden from ui::ListModelObserver:
  virtual void ListItemsAdded(size_t start, size_t count) OVERRIDE;
  virtual void ListItemsRemoved(size_t start, size_t count) OVERRIDE;
  virtual void ListItemMoved(size_t index, size_t target_index) OVERRIDE;
  virtual void ListItemsChanged(size_t start, size_t count) OVERRIDE;

  AppListModel::SearchResults* results_;   // Weak. Owned by AppListModel.
  scoped_nsobject<NSTableView> table_view_;
  ScopedVector<ItemObserver> item_observers_;

  DISALLOW_COPY_AND_ASSIGN(AppsSearchResultsModelBridge);
};

}  // namespace app_list

#endif  // UI_APP_LIST_COCOA_APPS_SEARCH_RESULTS_MODEL_BRIDGE_H_
