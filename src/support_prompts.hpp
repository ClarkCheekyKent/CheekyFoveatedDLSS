#pragma once
#include <string>

namespace cheeky::foveated_dlss {
struct SupportPrompts { std::string problem, steps; };
inline SupportPrompts support_prompts(const std::string& game, bool openxr_activity,
                                     const std::string& runtime) {
    const std::string mode = openxr_activity ? "VR (OpenXR activity detected)"
        : "Not detected - confirm desktop or VR";
    std::string problem = "**Game executable:** " + game + "\n\n**Mode:** " + mode;
    if (!runtime.empty()) problem += "\n\n**OpenXR runtime:** " + runtime;
    problem += "\n\n**Headset:** Not detected - enter model if using VR; otherwise N/A"
        "\n\n**What I expected:** [Describe the expected result]"
        "\n\n**What actually happened:** [Describe the symptom or error]"
        "\n\n**How often it happens:** [Every time / sometimes / once]"
        "\n\n**With the add-on disabled:** Not tested";
    const std::string launch_mode = openxr_activity ? "VR" : "[desktop / VR]";
    return {problem,
        "1. Launch " + game + " in " + launch_mode + " mode.\n"
        "2. Load [track, level, save, or scene].\n"
        "3. [Any relevant setting changes; captured settings are listed below].\n"
        "4. [Action that triggers the problem].\n"
        "5. Observe [the specific symptom].\n\n"
        "**Workaround, if any:** [Describe it, or write None known]"};
}
}
