NVIDIA NGX SDK headers and Windows x64 static libraries obtained from the official
NVIDIA/DLSS repository on 2026-10-16:

https://github.com/NVIDIA/DLSS/tree/main/include
https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/x64

`nvsdk_ngx_s.lib` is the Release /MT build; `nvsdk_ngx_s_dbg.lib` is Debug /MTd.
The accompanying NVIDIA RTX SDK license is preserved in LICENSE.txt and is
included in the locally built OpenXR installer and UEVR package.

The public SDK entry points are statically linked into the OpenXR layer. They
must not be resolved by searching the feature DLL for public SDK wrapper names.
The application supplies its game directory as the NGX feature search path.

Opt-in hardware integration probe (does not start a game or OpenXR session):

    CheekyOpenXRBindingTests.exe --fg-capability GAME_BINARY_DIRECTORY
    CheekyOpenXRBindingTests.exe --fg-generate GAME_BINARY_DIRECTORY bgra foveated

The second command exercises production worker inference and reads back the
generated texture. It requires an NVIDIA FG-capable GPU and nvngx_dlssg.dll in
the specified game directory. The ordinary WARP tests do not invoke NGX.
