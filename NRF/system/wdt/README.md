# Watchdog

Hardware watchdog (devicetree alias `watchdog0`) that resets the SoC when the application stops being healthy.
Enable with `CONFIG_SENSEI_WDT=y` (needs `CONFIG_SENSEI`).

`sensei_wdt_start(timeout_s, healthy, wq)` installs the timeout and starts a delayable work item that feeds the
watchdog every `CONFIG_SENSEI_WDT_FEED_PERIOD_MS` (default 5000), but only while `healthy()` returns true. Pass
NULL for `healthy` to always feed, and a work queue (or NULL for the system queue) to run the feed on. The
timeout must be longer than the feed period. The watchdog pauses while a debugger halts the CPU.

```c
#include "wdt/sensei_wdt.h"

static bool app_healthy(void) { return scheduler_progress(); }

sensei_wdt_start(CONFIG_SENSEI_WDT_TIMEOUT_S, app_healthy, NULL);
```

Untested on hardware.
