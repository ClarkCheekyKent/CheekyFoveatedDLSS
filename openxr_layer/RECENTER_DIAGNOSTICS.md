# Recenter and FG recreation diagnostics

Build the x64 Release OpenXR layer with `/p:CheekyFgLifetimeDiagnostics=true`.
The same property on CheekyOpenXRBindingTests enables its diagnostic forwarding
and invalid-handle probe regression (`--fg-lifetime`, also in the default run).
Normal builds include the frame-space ownership fix. They omit the extra
lifetime logs and failure probes.

The OpenXR startup log identifies instrumented builds with
`FG-LIFETIME diagnostic build active`.

- `FG-LIFETIME create_reference_space` / `create_action_space` record application
  space handles and owning sessions. Destroy ENTER and RETURN records bracket
  the original downstream call and retain its result. These hooks observe
  lifetime; they do not defer destruction or repair a stale frame.
- `reference_space_change` records runtime recenter events and change times.
- On failed runtime EndFrame, `submitted_layer` records each layer space and a
  self-locate probe result. `submitted_chain` records submitted swapchains and
  their read-only enumeration results. These probes run after failure only.
- `FG-FEATURE resize` records worker/feature identities, eye/pass, previous and
  new input sizes, old handle, format, guide size, configuration and crop.
  `FG-FEATURE create` records the resulting handle and NGX result.

Reproduce once after confirming the startup marker. If the image freezes,
leave it frozen briefly, then toggle Cheeky FG off/on if needed and export a
support bundle promptly. The bundle includes the startup trace. Correlate a
failed submitted handle with the destroy interval; do not infer a deadlock
from XR_ERROR_HANDLE_INVALID alone. Successful probes after the error do not
exclude a concurrent lifetime race.

The separate Standalone Host UI uses grouped FG metric tables. Full raw JSON
remains in support bundles and behind the Copy FG diagnostic JSON button.
