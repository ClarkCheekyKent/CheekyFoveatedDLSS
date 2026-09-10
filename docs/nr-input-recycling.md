# Before-mode input recycling

NR feature/resource use records are collected independently of view retirement,
including on stable feature/resource cache hits. A completed record releases its
fence once; an unsubmitted use, pending queue signal, or incomplete fence stays
retained. The internal `src/dlss_nr_lifetime.cpp` implementation is shared with
the native tests and registered through the production source globs in MSBuild
and CMake. Run `bin/Release/CheekyTests.exe --nr-lifetime` for 1,000 two-view
submit/complete/collect cycles plus blocked-GPU and failed-signal checks. These
tests use WARP and do not substitute the NVIDIA evaluator for lifetime coverage.

Before mode owns up to eight private inputs per view. A recorded input must stay
unavailable until the command list has been submitted and its GPU work has finished.

Submission matching uses an object-private-data token as well as pointer equality.
This allows a command-list wrapper and the native object exposed by ReShade to
match when the wrapper forwards D3D12 object private data. A retained recording
reference prevents object destruction before submission is observed. If private
data is unsupported, matching falls back to the original pointer; it never guesses
that an unmatched list has been submitted.

ReShade's execute-command-list event precedes ExecuteCommandLists. It records the
submitting queue; the present callback signals the completion fence on that queue
after submission. Both the queue association and the fence must clear before an
input is reused. NR feature/resource lifetimes follow the same notification rule.
The internal D3D11 transport, which notifies after its own ExecuteCommandLists,
explicitly requests immediate signaling.

Run `bin/Release/CheekyTests.exe --d3d12-composite` for the WARP regression. It
exercises production input allocation and submission bookkeeping with a forwarded
object alias. Before the fix, it failed after filling the eight-input pool. It now
runs 24 submissions with both aliased and identical pointers, and checks that a
full pool stays unavailable before submission and while GPU execution is blocked.
The test substitutes the NVIDIA evaluator; in-game NVIDIA validation is separate.

The game log reports `Before submission matched` (including whether pointers
differed), `Before input recycled`, and rate-limited `Before input unavailable`.
These distinguish successful resource reuse from the earlier silent fallback.
The September 7 in-game retest confirmed `aliased=yes` for both eyes. Before mode
continued from 13:10:33 through 13:11:38, with input reuse counts reaching 1,800
per resource and no input-unavailable messages in the captured session. This
validates sustained recycling in that run; it does not measure a performance
improvement or establish that the separate visual glitches are resolved.
