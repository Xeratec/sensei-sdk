# Energy markers

Two GPIO outputs for the digital inputs of a power analyzer (for example the Nordic PPK2), so that a current
trace shows which job runs. Enable with `CONFIG_SENSEI_MARKER=y` (needs `CONFIG_SENSEI`).

The pins are the devicetree aliases `marker0` and `marker1`, each a GPIO node with `gpios`. A pin is high while
at least one of its sources (bit 0 to 31) is on, so overlapping jobs give one marker.

```c
#include "marker/sensei_marker.h"

sensei_marker_init();            /* both pins low */
sensei_marker_set(0, 2, true);   /* pin 0, source 2 on */
sensei_marker_set(0, 2, false);
```

Untested on hardware.
