# Native Android Runtime

This directory will hold the game-agnostic native Android runtime and platform abstraction layer
(Android NDK / GameActivity, Vulkan or OpenGL ES renderer backend, AAudio/OpenSL ES audio backend,
input mapping, and POSIX/Android filesystem adapters).

Keep all game-specific logic out of `runtime/`; decompiled game functions belong under
`game/<system>/`.
