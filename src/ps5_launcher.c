#include <stdio.h>

#include "wkali.h"
#include "ps5_launcher.h"

extern int sceSystemServiceLaunchWebBrowser(const char *uri);

int ps5_launch_browser(const char *uri) {
  wkali_log("[WKALI] Launching browser: %s\n", uri);
  if (sceSystemServiceLaunchWebBrowser(uri) != 0) {
    wkali_notify("WKALI: Failed to launch browser.");
    return -1;
  }
  return 0;
}
