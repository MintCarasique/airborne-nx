# shaders_native

Xbox 360 shader translator for Medal of Honor: Airborne. Copied from `tools/reference-nfsmw/shaders` (GPL-3.0, XenosRecomp fork); the reference copy is left untouched. Changes are marked `AirborneNX:` in the code:

1. Texture fetches on samplers missing from the constant table get their `sN_*DescriptorIndex` / `sN_InvTamano` defines emitted by the translator.
2. Duplicate vertex elements (same usage/index) are declared once.
3. A vfetch with no declared element becomes a float4 `iVfetch<addr>` input (TEXCOORD, free index); exports to undeclared interpolators (memexport) go to a dummy local.
4. 7e3 render-target output clamp (`salida7e3`, spec bits 5, 6, 7, 10) before pixel shader return.

Build: see `rex/out/native_shaders_v2/build_library.sh`.
