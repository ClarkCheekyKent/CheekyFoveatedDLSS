# SignPath setup

This first workflow validates GitHub origin verification and signing with the
self-signed test certificate. It builds the OpenXR installer and produces separate
UEVR and Standalone ZIPs. It does not publish a GitHub release.

## Account and project

1. Accept the OSS organization invitation before confirming the CI user's email.
2. Use the existing project with slug `CheekyFoveatedDLSS` and signing policy
   `test-signing`. Leave `release-signing` alone while its certificate is pending.
3. Under **Trusted Build Systems**, link **GitHub.com** to the project. If it is
   missing, add the predefined GitHub.com trusted build system at organization
   level first. The GitLab.com link does not cover GitHub Actions.
4. Install the [SignPath GitHub App](https://github.com/apps/signpath) for
   `ClarkCheekyKent/CheekyFoveatedDLSS` if needed for your organization setup.
5. Add an artifact configuration with slug **`binaries`**, using the custom XML
   from [.github/signpath/binaries.xml](.github/signpath/binaries.xml).
   It signs seven explicitly named, project-built PE files inside one ZIP,
   covering UEVR, Standalone and the shared OpenXR layer. ReShade and OptiScaler
   binaries are excluded from this signing bundle.
   Add a second configuration with slug **`installer`**, using
   [.github/signpath/installer.xml](.github/signpath/installer.xml).
   Check that the `test-signing` policy allows both configurations.
6. Confirm the CI user's email, grant it submitter access to the test signing
   policy, and create its API token. Keep the token out of chat and source files.

## GitHub configuration

In repository **Settings > Secrets and variables > Actions**, set:

| Kind | Name | Value |
| --- | --- | --- |
| Variable | `SIGNPATH_ORGANIZATION_ID` | Organization ID from SignPath (not the project slug) |
| Secret | `SIGNPATH_API_TOKEN` | API token for the CI submitter |

Commit the workflow, XML, and staging script to the default branch. Under
**Actions > SignPath test signing > Run workflow**, first leave **sign** disabled.
The workflow compiles production targets on a GitHub-hosted Windows runner and
uploads `signpath-unsigned-binaries`. Its downloaded ZIP is an artifact sample
you can upload to SignPath if you prefer generating a configuration from a sample;
ensure the resulting configuration uses the same exact file paths as the XML.

Run again with **sign** enabled after completing the portal configuration.
Approve the signing request in SignPath if the policy requires it. A successful
run performs these steps in order:

1. Sign the seven built binaries with the `binaries` configuration.
2. Check that every downloaded binary has an Authenticode signature and the same
   signer. Self-signed certificate trust failures are allowed for this test
   (`NotTrusted`, or the specific untrusted-root message reported as `UnknownError`);
   missing signatures, hash mismatches and other verification errors stop the job.
3. Build `CheekyOpenXRSetup.exe` with the signed OpenXR layer and its manifest,
   using Inno Setup supplied by the GitHub-hosted Windows image.
4. Sign the installer with the `installer` configuration in a second request.
5. Check the installer's signature and require the same certificate as the layer.
6. Package signed inputs into **two separate ZIPs**, retaining the existing
   installation layouts. Generate `SHA256SUMS.txt` from the final archives.

Download `signpath-TEST-SIGNED-packages` from the run. This GitHub artifact contains:

- `CheekyFoveatedDLSS-<version>-UEVR-TEST-SIGNED.zip`
- `CheekyFoveatedDLSS-<version>-Standalone-TEST-SIGNED.zip`
- `SHA256SUMS.txt`

Both packages contain the same signed OpenXR installer. The intermediate signing
ZIPs are only transport containers, not replacements for the release packages.
Packaging reads explicit signed download paths without replacing local Release
binaries. An unsuccessful signing request prevents final packaging.

Inspect both requests' origin metadata and the files' Digital Signatures
properties. The test certificate is self-signed and is not expected to be publicly
trusted by Windows. These packages are labeled `TEST-SIGNED` for testing.

For a local configuration sample from existing Release outputs:

```powershell
./scripts/stage-signpath.ps1
```

This prints a unique `build/signpath-*.zip` path. Local samples help configure the
artifact format; they are not a substitute for a GitHub-built signing request.
In CI, upload the staged directory directly: `upload-artifact@v4` creates the ZIP.

## Production follow-up

After a successful test request, send Phillip the workflow/run and signing-request
links for the setup review. SignPath will arrange the production certificate.
Production signing still needs a release policy/workflow and verification of
public certificate trust; this workflow only requests `test-signing`.

## Local checks

Run `./tests/signpath_packaging_tests.ps1` to check package source isolation,
both ZIP layouts, final checksums, and rejected signature/missing-file cases.
The test uses synthetic files and mocked certificate results for the successful
packaging path; it does not contact SignPath or prove real certificate validity.
The ordinary package scripts retain their existing defaults and also accept
`-BinaryRoot` and `-OpenXRSetupPath` for explicit inputs. The signed orchestration
script `scripts/package-signpath.ps1` requires signature verification before
calling them.

References: [GitHub integration](https://docs.signpath.io/trusted-build-systems/github),
[artifact configuration](https://docs.signpath.io/artifact-configuration/),
[ZIP signing examples](https://docs.signpath.io/artifact-configuration/examples).
