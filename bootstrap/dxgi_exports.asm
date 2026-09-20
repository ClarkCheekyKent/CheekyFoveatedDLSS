; Win64 transparent forwarding, including undocumented DXGI entry points.
; Tail jumps preserve stack arguments and every register used for parameters.
; A C++ "void()" forwarder is not ABI-correct for these varying signatures.
option casemap:none
EXTERN cheeky_dxgi_targets:QWORD
EXTERN cheeky_dxgi_resolve:PROC

.code
include proxy_forward.inc

FORWARD cheeky_proxy_ApplyCompatResolutionQuirking, 0, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_CompatString, 1, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_CompatValue, 2, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_DXGIDumpJournal, 3, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_PIXBeginCapture, 4, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_PIXEndCapture, 5, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_PIXGetCaptureState, 6, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_SetAppCompatStringPointer, 7, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_UpdateHMDEmulationStatus, 8, cheeky_dxgi_targets, cheeky_dxgi_resolve
; Slots 9-11 have typed C++ wrappers to finish bootstrap after a successful
; real factory call, before the application can create its first swap chain.
FORWARD cheeky_proxy_DXGID3D10CreateDevice, 12, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_DXGID3D10CreateLayeredDevice, 13, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_DXGID3D10GetLayeredDeviceSize, 14, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_DXGID3D10RegisterLayers, 15, cheeky_dxgi_targets, cheeky_dxgi_resolve
; Slots 16 and 18 have typed wrappers to guard third-party hook recursion.
FORWARD cheeky_proxy_DXGIDisableVBlankVirtualization, 17, cheeky_dxgi_targets, cheeky_dxgi_resolve
FORWARD cheeky_proxy_DXGIReportAdapterConfiguration, 19, cheeky_dxgi_targets, cheeky_dxgi_resolve
END
