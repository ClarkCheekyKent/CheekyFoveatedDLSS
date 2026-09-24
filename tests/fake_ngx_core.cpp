// Compile the existing synthetic NGX implementation as a core fixture. Real
// core runtimes do not expose the SR snippet marker; keep its test helper under
// another name. This never links or executes a vendor's binary.
#define NVSDK_NGX_GetSnippetVersion CheekyFakeSnippetVersion
#include "fake_ngx.cpp"
