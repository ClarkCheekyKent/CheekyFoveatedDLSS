# SR motion resource lifetime

SR crop-motion passes own a recording lifetime shared with the native D3D12
observer. They remain live until successful command-list Reset or destruction
retires the recording and all submitted uses on every queue complete. A
completed first execution does not make a replayable recording reusable.

Previously, motion passes matched a persistent command-list identity and kept
only their first submitting queue/fence. Collection after native submissions
could reuse or destroy resources without accounting for later recordings or
replay. Three Silent Hill f heap-corruption dump stack scans consistently
included motion-pass descriptor-heap release. This motivates the fix; the
original corrupting write has not been established by those scans.

The owner takes the execution mutex before its cache mutex. Submission and
Reset accounting use the existing recording tracker under that execution
mutex. If native observation or private recording identity is unavailable,
correction is declined before GPU commands are recorded.

`CheekyNrObserverTests` exercises actual SR motion resources above the cache
limit, repeated execution, a blocked second queue, Reset/re-record on the same
list, and destruction. It also retains the NR alias, failed-signal and lifetime
checks. Run it with and without `CHEEKY_NR_TEST_NO_DEBUG_LAYER=1`, plus
`CheekyTests --motion-resample` and the UEVR compatibility modes.

Silent Hill f runtime validation is pending. Keep the same profile and exercise
startup, enabling foveation in game, and transitions/resolution changes; compare
with the bridge using the existing switcher. The blink fix is unchanged.
