# NVIDIA Optical Flow interface headers

API 5.0 headers, Copyright 2018-2024 NVIDIA Corporation & Affiliates. The
original MIT notice is retained in every header and in LICENSE.txt. These
headers do not license the SDK's samples or runtime binaries.

Source snapshot retrieved 2026-10-24 from
https://github.com/AlanBacker/VRChat-DLSS5-Cam/tree/main/third_party/nvof.
The API declarations were also compared with the SDK 5.0 header snapshot at
https://github.com/SlimRG/NvidiaOF5. No third-party implementation was copied.

SHA-256 of the retained header bytes:

| File | SHA-256 |
| --- | --- |
| nvOpticalFlowCommon.h | F133879F7ADD54D42D71F5D7839A05962AF7AAA3620909E3CBFBECE77119D30F |
| nvOpticalFlowD3D11.h | 4721A36321591E6882E68AD342F5113A64E6C55F066B563E205FD9CC1A91CAE3 |
| nvOpticalFlowD3D12.h | 0B9E114F8E05140C6CD551EF1A7BF772462DFCD9424045081910F57CB68CCD33 |

The application loads nvofapi64.dll from Windows System32. NVIDIA installs
that DLL with its display driver; it is not distributed with Cheeky.
