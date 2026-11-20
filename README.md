# AI Limit VR

Native stereo VR mod for AI LIMIT (Steam app 2407270, Sense Games), a third person souls-like in Unity 2022.3 (IL2CPP, HDRP). Headset at the game's third person camera, gamepad, seated, OpenXR. Work in progress, Phase 1 (not released).

## Starting point

`from_cuphead/native/` is a copy of Cuphead VR's native OpenXR DLL source (github.com/scubasteve2365/CupheadVR, MIT, same author) at commit cff6522, version 1.05: swapchains, submit on the render thread, logging, start-failure handling, render events through `GL.IssuePluginEvent`. Cuphead is a Mono game with the built-in renderer, so only the OpenXR side and the design carry over.

MIT licence, see LICENSE.
