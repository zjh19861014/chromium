// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WEB_APPLICATIONS_COMPONENTS_INSTALL_OPTIONS_H_
#define CHROME_BROWSER_WEB_APPLICATIONS_COMPONENTS_INSTALL_OPTIONS_H_

#include <iosfwd>

#include "url/gurl.h"

namespace web_app {

enum class InstallSource;
enum class LaunchContainer;

struct InstallOptions {
  InstallOptions(const GURL& url,
                 LaunchContainer launch_container,
                 InstallSource install_source);
  ~InstallOptions();
  InstallOptions(const InstallOptions& other);
  InstallOptions(InstallOptions&& other);
  InstallOptions& operator=(const InstallOptions& other);

  bool operator==(const InstallOptions& other) const;

  GURL url;
  LaunchContainer launch_container;
  InstallSource install_source;

  // If true, a shortcut is added to the Applications folder on macOS, and Start
  // Menu on Linux and Windows. On Chrome OS, all installed apps show up in the
  // app list, so there is no need to do anything there. If false, we skip
  // adding a shortcut to desktop as well, regardless of the value of
  // |add_to_desktop|.
  // TODO(ortuno): Make adding a shortcut to the applications menu independent
  // from adding a shortcut to desktop.
  bool add_to_applications_menu = true;

  // If true, a shortcut is added to the desktop on Linux and Windows. Has no
  // effect on macOS and Chrome OS.
  bool add_to_desktop = true;

  // If true, a shortcut is added to the "quick launch bar" of the OS: the Shelf
  // for Chrome OS, the Dock for macOS, and the Quick Launch Bar or Taskbar on
  // Windows. Currently this only works on Chrome OS.
  bool add_to_quick_launch_bar = true;

  // Whether the app should be reinstalled even if the user has previously
  // uninstalled it.
  bool override_previous_user_uninstall = false;

  // This must only be used by pre-installed default or system apps that are
  // valid PWAs if loading the real service worker is too costly to verify
  // programmatically.
  bool bypass_service_worker_check = false;

  // This should be used for installing all default apps so that good metadata
  // is ensured.
  bool require_manifest = false;

  // Whether the app should be reinstalled even if it is already installed.
  bool always_update = false;

  // Whether a placeholder app should be installed if we fail to retrieve the
  // metadata for the app. A placeholder app uses:
  //  - The default Chrome App icon for the icon
  //  - |url| as the start_url
  //  - |url| as the app name
  bool install_placeholder = false;
};

std::ostream& operator<<(std::ostream& out,
                         const InstallOptions& install_options);

}  // namespace web_app

#endif  // CHROME_BROWSER_WEB_APPLICATIONS_COMPONENTS_INSTALL_OPTIONS_H_
