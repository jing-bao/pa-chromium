// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/threading/platform_thread.h"
#include "cloud_print/gcp20/prototype/dns_sd_server.h"

int main(int argc, char* argv[]) {
  CommandLine::Init(argc, argv);

  logging::InitLogging(NULL,
                       logging::LOG_ONLY_TO_SYSTEM_DEBUG_LOG,
                       logging::LOCK_LOG_FILE,
                       logging::APPEND_TO_OLD_LOG_FILE,
                       logging::DISABLE_DCHECK_FOR_NON_OFFICIAL_RELEASE_BUILDS);

  DnsSdServer dns_sd_server;
  dns_sd_server.Start();
  base::PlatformThread::Sleep(base::TimeDelta::FromSeconds(2));
  dns_sd_server.Shutdown();

  return 0;
}

