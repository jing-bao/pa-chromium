// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * WebUI to monitor the Sync File System Service.
 */
var SyncService = (function() {
'use strict';

var SyncService = {};

/**
 * Request Sync Service Status.
 */
function getServiceStatus() {
  chrome.send('getServiceStatus');
}

/**
 * Called when service status is initially retrieved or updated via events.
 * @param {string} Service status enum as a string.
 */
SyncService.onGetServiceStatus = function(statusString) {
  $('service-status').textContent = statusString;
}

/**
 * Request Google Drive Notification Source. e.g. XMPP or polling.
 */
function getNotificationSource() {
  chrome.send('getNotificationSource');
}

/**
 * Handles callback from getNotificationSource.
 * @param {string} Notification source as a string.
 */
SyncService.onGetNotificationSource = function(sourceString) {
  $('notification-source').textContent = sourceString;
}

/**
 * Creates an element named |elementName| containing the content |text|.
 * @param {string} elementName Name of the new element to be created.
 * @param {string} text Text to be contained in the new element.
 * @return {HTMLElement} The newly created HTML element.
 */
function createElementFromText(elementName, text) {
  var element = document.createElement(elementName);
  element.appendChild(document.createTextNode(text));
  return element;
}

/**
 * Request debug log.
 */
function getLog() {
  chrome.send('getLog');
}

/**
 * Handles callback from getUpdateLog.
 * @param {Array} list List of dictionaries containing 'time' and 'logEvent'.
 */
SyncService.onGetLog = function(logEntries) {
  var itemContainer = $('log-entries');
  for (var i = 0; i < logEntries.length; i++) {
    var logEntry = logEntries[i];
    var tr = document.createElement('tr');
    tr.appendChild(createElementFromText('td', logEntry.time));
    tr.appendChild(createElementFromText('td', logEntry.logEvent));
    itemContainer.appendChild(tr);
  }
}

/**
 * Get initial sync service values and set listeners to get updated values.
 */
function main() {
  cr.ui.decorate('tabbox', cr.ui.TabBox);
  getServiceStatus();
  getNotificationSource();

  // TODO: Look for a way to push entries to the page when necessary.
  window.setInterval(getLog, 1000);
}

document.addEventListener('DOMContentLoaded', main);
return SyncService;
})();
