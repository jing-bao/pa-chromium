// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/unload_controller.h"

#include "base/logging.h"
#include "base/message_loop.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "chrome/common/chrome_notification_types.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"

namespace chrome {


////////////////////////////////////////////////////////////////////////////////
// DetachedWebContentsDelegate will delete web contents when they close.
class UnloadController::DetachedWebContentsDelegate
    : public content::WebContentsDelegate {
 public:
  DetachedWebContentsDelegate() { }
  virtual ~DetachedWebContentsDelegate() { }

 private:
  // WebContentsDelegate implementation.
  virtual bool ShouldSuppressDialogs() OVERRIDE {
    return true;  // Return true so dialogs are suppressed.
  }

  virtual void CloseContents(content::WebContents* source) OVERRIDE {
    // Finished detached close.
    // UnloadController will observe |NOTIFICATION_WEB_CONTENTS_DISCONNECTED|.
    delete source;
  }

  DISALLOW_COPY_AND_ASSIGN(DetachedWebContentsDelegate);
};

////////////////////////////////////////////////////////////////////////////////
// UnloadController, public:

UnloadController::UnloadController(Browser* browser)
    : browser_(browser),
      tab_needing_before_unload_ack_(NULL),
      is_attempting_to_close_browser_(false),
      detached_delegate_(new DetachedWebContentsDelegate()),
      weak_factory_(this) {
  browser_->tab_strip_model()->AddObserver(this);
}

UnloadController::~UnloadController() {
  browser_->tab_strip_model()->RemoveObserver(this);
}

bool UnloadController::CanCloseContents(content::WebContents* contents) {
  // Don't try to close the tab when the whole browser is being closed, since
  // that avoids the fast shutdown path where we just kill all the renderers.
  return !is_attempting_to_close_browser_;
}

bool UnloadController::BeforeUnloadFired(content::WebContents* contents,
                                         bool proceed) {
  if (!is_attempting_to_close_browser_) {
    if (!proceed) {
      contents->SetClosedByUserGesture(false);
    } else {
      // No more dialogs are possible, so remove the tab and finish
      // running unload listeners asynchrounously.
      browser_->tab_strip_model()->delegate()->CreateHistoricalTab(contents);
      DetachWebContents(contents);
    }
    return proceed;
  }

  if (!proceed) {
    CancelWindowClose();
    contents->SetClosedByUserGesture(false);
    return false;
  }

  if (tab_needing_before_unload_ack_ == contents) {
    // Now that beforeunload has fired, queue the tab to fire unload.
    tab_needing_before_unload_ack_ = NULL;
    tabs_needing_unload_.insert(contents);
    ProcessPendingTabs();
    // We want to handle firing the unload event ourselves since we want to
    // fire all the beforeunload events before attempting to fire the unload
    // events should the user cancel closing the browser.
    return false;
  }

  return true;
}

bool UnloadController::ShouldCloseWindow() {
  if (HasCompletedUnloadProcessing())
    return true;

  is_attempting_to_close_browser_ = true;

  if (!TabsNeedBeforeUnloadFired())
    return true;

  ProcessPendingTabs();
  return false;
}

bool UnloadController::TabsNeedBeforeUnloadFired() {
  if (!tabs_needing_before_unload_.empty() ||
      tab_needing_before_unload_ack_ != NULL)
    return true;

  if (!tabs_needing_unload_.empty())
    return false;

  for (int i = 0; i < browser_->tab_strip_model()->count(); ++i) {
    content::WebContents* contents =
        browser_->tab_strip_model()->GetWebContentsAt(i);
    if (contents->NeedToFireBeforeUnload())
      tabs_needing_before_unload_.insert(contents);
  }
  return !tabs_needing_before_unload_.empty();
}

////////////////////////////////////////////////////////////////////////////////
// UnloadController, content::NotificationObserver implementation:

void UnloadController::Observe(int type,
                               const content::NotificationSource& source,
                               const content::NotificationDetails& details) {
  switch (type) {
    case content::NOTIFICATION_WEB_CONTENTS_DISCONNECTED: {
      registrar_.Remove(this,
                        content::NOTIFICATION_WEB_CONTENTS_DISCONNECTED,
                        source);
      content::WebContents* contents =
          content::Source<content::WebContents>(source).ptr();
      ClearUnloadState(contents);
      break;
    }
    default:
      NOTREACHED() << "Got a notification we didn't register for.";
  }
}

////////////////////////////////////////////////////////////////////////////////
// UnloadController, TabStripModelObserver implementation:

void UnloadController::TabInsertedAt(content::WebContents* contents,
                                     int index,
                                     bool foreground) {
  TabAttachedImpl(contents);
}

void UnloadController::TabDetachedAt(content::WebContents* contents,
                                     int index) {
  TabDetachedImpl(contents);
}

void UnloadController::TabReplacedAt(TabStripModel* tab_strip_model,
                                     content::WebContents* old_contents,
                                     content::WebContents* new_contents,
                                     int index) {
  TabDetachedImpl(old_contents);
  TabAttachedImpl(new_contents);
}

void UnloadController::TabStripEmpty() {
  // Set is_attempting_to_close_browser_ here, so that extensions, etc, do not
  // attempt to add tabs to the browser before it closes.
  is_attempting_to_close_browser_ = true;
}

////////////////////////////////////////////////////////////////////////////////
// UnloadController, private:

void UnloadController::TabAttachedImpl(content::WebContents* contents) {
  // If the tab crashes in the beforeunload or unload handler, it won't be
  // able to ack. But we know we can close it.
  registrar_.Add(
      this,
      content::NOTIFICATION_WEB_CONTENTS_DISCONNECTED,
      content::Source<content::WebContents>(contents));
}

void UnloadController::TabDetachedImpl(content::WebContents* contents) {
  if (tabs_needing_unload_ack_.find(contents) !=
      tabs_needing_unload_ack_.end()) {
    // Tab needs unload to complete.
    // It will send |NOTIFICATION_WEB_CONTENTS_DISCONNECTED| when done.
    return;
  }

  // If WEB_CONTENTS_DISCONNECTED was received then the notification may have
  // already been unregistered.
  const content::NotificationSource& source =
      content::Source<content::WebContents>(contents);
  if (registrar_.IsRegistered(this,
                              content::NOTIFICATION_WEB_CONTENTS_DISCONNECTED,
                              source)) {
    registrar_.Remove(this,
                      content::NOTIFICATION_WEB_CONTENTS_DISCONNECTED,
                      source);
  }

  if (is_attempting_to_close_browser_)
    ClearUnloadState(contents);
}

bool UnloadController::DetachWebContents(content::WebContents* contents) {
  int index = browser_->tab_strip_model()->GetIndexOfWebContents(contents);
  if (index != TabStripModel::kNoTab &&
      contents->NeedToFireBeforeUnload()) {
    tabs_needing_unload_ack_.insert(contents);
    browser_->tab_strip_model()->DetachWebContentsAt(index);
    contents->SetDelegate(detached_delegate_.get());
    contents->OnUnloadDetachedStarted();
    return true;
  }
  return false;
}

void UnloadController::ProcessPendingTabs() {
  if (!is_attempting_to_close_browser_) {
    // Because we might invoke this after a delay it's possible for the value of
    // is_attempting_to_close_browser_ to have changed since we scheduled the
    // task.
    return;
  }

  if (tab_needing_before_unload_ack_ != NULL) {
    // Wait for |BeforeUnloadFired| before proceeding.
    return;
  }

  // Process a beforeunload handler.
  if (!tabs_needing_before_unload_.empty()) {
    WebContentsSet::iterator it = tabs_needing_before_unload_.begin();
    content::WebContents* contents = *it;
    tabs_needing_before_unload_.erase(it);
    // Null check render_view_host here as this gets called on a PostTask and
    // the tab's render_view_host may have been nulled out.
    if (contents->GetRenderViewHost()) {
      tab_needing_before_unload_ack_ = contents;
      contents->OnCloseStarted();
      contents->GetRenderViewHost()->FirePageBeforeUnload(false);
    } else {
      ProcessPendingTabs();
    }
    return;
  }

  // Process all the unload handlers. (The beforeunload handlers have finished.)
  if (!tabs_needing_unload_.empty()) {
    browser_->OnWindowClosing();

    // Run unload handlers detached since no more interaction is possible.
    WebContentsSet::iterator it = tabs_needing_unload_.begin();
    while (it != tabs_needing_unload_.end()) {
      WebContentsSet::iterator current = it++;
      content::WebContents* contents = *current;
      tabs_needing_unload_.erase(current);
      // Null check render_view_host here as this gets called on a PostTask
      // and the tab's render_view_host may have been nulled out.
      if (contents->GetRenderViewHost()) {
        contents->OnUnloadStarted();
        DetachWebContents(contents);
        contents->GetRenderViewHost()->ClosePage();
      }
    }

    // Get the browser hidden.
    if (browser_->tab_strip_model()->empty()) {
      browser_->TabStripEmpty();
    } else {
      browser_->tab_strip_model()->CloseAllTabs();  // tabs not needing unload
    }
    return;
  }

  if (HasCompletedUnloadProcessing()) {
    browser_->OnWindowClosing();

    // Get the browser closed.
    if (browser_->tab_strip_model()->empty()) {
      browser_->TabStripEmpty();
    } else {
      // There may be tabs if the last tab needing beforeunload crashed.
      browser_->tab_strip_model()->CloseAllTabs();
    }
    return;
  }
}

bool UnloadController::HasCompletedUnloadProcessing() const {
  return is_attempting_to_close_browser_ &&
      tabs_needing_before_unload_.empty() &&
      tab_needing_before_unload_ack_ == NULL &&
      tabs_needing_unload_.empty() &&
      tabs_needing_unload_ack_.empty();
}

void UnloadController::CancelWindowClose() {
  // Closing of window can be canceled from a beforeunload handler.
  DCHECK(is_attempting_to_close_browser_);
  tabs_needing_before_unload_.clear();
  if (tab_needing_before_unload_ack_ != NULL) {
    tab_needing_before_unload_ack_->OnCloseCanceled();
    tab_needing_before_unload_ack_ = NULL;
  }
  for (WebContentsSet::iterator it = tabs_needing_unload_.begin();
       it != tabs_needing_unload_.end(); it++) {
    content::WebContents* contents = *it;
    contents->OnCloseCanceled();
  }
  tabs_needing_unload_.clear();

  // No need to clear tabs_needing_unload_ack_. Those tabs are already detached.

  is_attempting_to_close_browser_ = false;

  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_BROWSER_CLOSE_CANCELLED,
      content::Source<Browser>(browser_),
      content::NotificationService::NoDetails());
}

void UnloadController::ClearUnloadState(content::WebContents* contents) {
  if (tabs_needing_unload_ack_.erase(contents) > 0) {
    if (HasCompletedUnloadProcessing())
      PostTaskForProcessPendingTabs();
    return;
  }

  if (!is_attempting_to_close_browser_)
    return;

  if (tab_needing_before_unload_ack_ == contents) {
    tab_needing_before_unload_ack_ = NULL;
    PostTaskForProcessPendingTabs();
    return;
  }

  if (tabs_needing_before_unload_.erase(contents) > 0 ||
      tabs_needing_unload_.erase(contents) > 0) {
    if (tab_needing_before_unload_ack_ == NULL)
      PostTaskForProcessPendingTabs();
  }
}

void UnloadController::PostTaskForProcessPendingTabs() {
  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&UnloadController::ProcessPendingTabs,
                 weak_factory_.GetWeakPtr()));
}

}  // namespace chrome
