# NR recording lifetimes and Before-mode input recycling

NR feature/resource retention and Before-mode private inputs share the internal
`src/dlss_nr_lifetime.cpp` implementation. A use belongs to a command-list
recording generation. The recording stays live until successful Reset or object
destruction, including when its first GPU execution has already finished.
Failed Reset keeps the generation live. Reset without execution discards work.

An object-private-data interface identifies the recording. Forwarding wrappers
and their native command list share this identity when they forward private data.
Its destruction notification retires the current generation without retaining the
command list. Before recording NR commands, the code verifies compatible native
observation through the list's device and verifies that private data round-trips.
Unsupported observation or identity skips NR with a diagnostic and uses SR fallback.

The resident native observer records every execution **after** ExecuteCommandLists,
independently of gaze, calibration, or the host integration. NR is no longer counted
by ReShade's pre-submit callbacks or explicit DX11 transport notifications. Native
DX12, Streamline, and the DX11 transport's internal DX12 list use this same path.

Each actual queue has its own fence timeline. Replaying a recording adds a new
completion requirement even if an earlier execution finished. A failed signal
retains the queue and requirement until signaling succeeds; another queue's fence
cannot satisfy it. Completed execution records and their fences are collected
once, including on active views and stable feature/resource cache hits. The
recording remains live for replay after those completed records are gone.

The three-input per-view limit includes retired inputs that still have pending work. Inputs
are reusable, and retained NR resources releasable, only after **both** recording
retirement and completion on every executing queue. Pool exhaustion falls back to
SR until reclamation is safe. No guessed submission or presentation queue is used.
Each view keeps at most two NR feature instances (active plus one alternate),
including instances still pending retirement. This covers toggling full/foveated
NR without retaining a history of slider choices. Retired features snapshot their
recording dependencies so later work on the same view cannot prevent collection.
Intermediate textures track their own recordings and use a six-entry per-view
cache: two configurations across three rotating input copies. On a cache miss,
the least recently used safe texture set is released before allocating another.
If all slots are still live, NR falls back to SR until safe reclamation instead
of growing either cache. Limits are entry counts, not a total VRAM byte budget;
NVIDIA's internal memory usage is not measured here.

The observer's recursive execution mutex serializes recording, Execute, Reset,
and collection, and is acquired before NR owner mutexes. The private-data
notification only retires its generation under the execution mutex; it does not
call back into resource owners. COM releases therefore cannot reenter a held NR
owner mutex. The observer pins its module and remains installed across host detach;
subsequent collection drains retired owners as completion becomes observable.

## Automated validation

- `bin/Release/CheekyTests.exe --nr-lifetime`: direct production bookkeeping with
  WARP, 1,000 two-view cycles, bounded/drained fence ownership, failed signaling,
  failed Reset results, discarded work, repeated execution, a blocked second queue,
  Reset during pending work, private-data rejection, wrapper aliases, and destruction.
- `bin/Release/CheekyNrObserverTests.exe`: the same lifetime checks with real native
  Execute/Reset hooks, plus the private-input/compositor suite. The ordinary
  gaze/calibration/timing consumers are inert so they cannot supply NR notifications.
  Private-input scenarios cover native DX12, Streamline, and DX11-transport frame
  routes, aliases, view release during blocked replay, three-slot backpressure,
  sustained recycling after Reset, and source-texture readback preservation.
- `scripts/build.ps1 -Configuration Release`: builds both integrations and runs
  the native/UEVR suites, including native DX12, Streamline, and DX11 late attachment.
  CMake registers the direct lifetime test and the native-observer executable too.

The input-copy tests substitute NVIDIA evaluation; lifetime tests exercise the
actual shared bookkeeping and real WARP queues directly. No live NVIDIA NR, game,
or headset validation was performed for these fixes. Those checks remain follow-up
work; passing route fixtures does not establish live transport or headset behavior.

The earlier September 7 game capture showed sustained aliased input recycling,
but preceded replay-safe retirement. It does not validate the updated lifetime
policy or establish that the separate visual glitches are resolved.
