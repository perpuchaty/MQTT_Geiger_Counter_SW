# OTA Artifacts

This folder is part of the repository and stores OTA-ready firmware binaries.

Build behavior:
- During `ESP-IDF: Build`, CMake copies `build/geiger_counter.bin` to `ota/geiger_counter_ota.bin`.
- The copy is performed by a post-build command in [CMakeLists.txt](../CMakeLists.txt).

Notes:
- The file is overwritten when the firmware content changes.
- Use `ota/geiger_counter_ota.bin` as the upload payload for your OTA server.
