# Local changes to PCSX2 in the graphics DLL

The graphics DLL is built from PCSX2 2.8.2 (see PINNED_REVISION) plus the bridge's own patch and these changes:

- `pcsx2/GS/Renderers/HW/GSRendererHW.cpp`: `Destroy`, `PurgeTextureCache`, `ReadbackTextureCache` and `Reset` check that
  `g_texture_cache` exists before using it. A reset or purge arriving while the cache does not exist read through a null pointer.
