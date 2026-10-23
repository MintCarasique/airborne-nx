param([switch]$CheckGenerated)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$previousPath=$env:PATH
Push-Location $projectRoot
try {
    $env:PATH="$projectRoot\tools\toolchain\llvm-mingw-20261020-ucrt-x86_64\bin;$env:PATH"
    clang++ -std=c++17 -O2 -Iruntime/switch tools/tests/test_rotate_compat.cpp -o analysis/instruction_tests/test_rotate_compat.exe
    if($LASTEXITCODE) { throw 'Switch GCC rotate compatibility test build failed' }
    & analysis/instruction_tests/test_rotate_compat.exe
    if($LASTEXITCODE) { throw 'Switch GCC rotate compatibility tests failed' }
    clang++ -std=c++17 -O1 -Wno-macro-redefined -DSIMDE_ENABLE_NATIVE_ALIASES -Itools/tests/fake_nx -Iruntime/switch -Igenerated/ppc -Itools/XenonRecomp/thirdparty/simde tools/tests/test_nx_mapped_arena.cpp -o analysis/instruction_tests/test_nx_mapped_arena.exe
    if($LASTEXITCODE) { throw 'Switch runtime mapped arena test build failed' }
    & analysis/instruction_tests/test_nx_mapped_arena.exe
    if($LASTEXITCODE) { throw 'Switch runtime mapped arena tests failed' }
    clang++ -std=c++17 -O1 -Wno-macro-redefined -DSIMDE_ENABLE_NATIVE_ALIASES -Itools/tests/fake_nx -Iruntime/switch -Igenerated/ppc -Itools/XenonRecomp/thirdparty/simde tools/tests/test_nx_shutdown_waits.cpp -o analysis/instruction_tests/test_nx_shutdown_waits.exe
    if($LASTEXITCODE) { throw 'Switch shutdown wait test build failed' }
    & analysis/instruction_tests/test_nx_shutdown_waits.exe
    if($LASTEXITCODE) { throw 'Switch shutdown wait tests failed' }
    python tools/tests/test_vector_translation.py
    if($LASTEXITCODE) { throw 'Vector translation tests failed' }
    python tools/tests/test_half_translation.py
    if($LASTEXITCODE) { throw 'FLOAT16_2 translation tests failed' }
    python tools/tests/test_blrl_translation.py
    if($LASTEXITCODE) { throw 'BLRL translation tests failed' }
    clang++ -std=c++17 -O2 -fno-builtin-roundevenf tools/tests/test_roundeven.cpp runtime/host/roundeven.cpp -o analysis/instruction_tests/test_roundeven.exe
    if($LASTEXITCODE) { throw 'Host roundevenf test build failed' }
    & analysis/instruction_tests/test_roundeven.exe
    if($LASTEXITCODE) { throw 'Host roundevenf tests failed' }
    clang++ -std=c++17 -O2 -Iruntime/host tools/tests/test_xex_header.cpp -o analysis/instruction_tests/test_xex_header.exe
    if($LASTEXITCODE) { throw 'XEX header test build failed' }
    & analysis/instruction_tests/test_xex_header.exe
    if($LASTEXITCODE) { throw 'XEX header tests failed' }
    clang++ -std=c++17 -O2 -Iruntime/host tools/tests/test_virtual_memory.cpp -o analysis/instruction_tests/test_virtual_memory.exe
    if($LASTEXITCODE) { throw 'Guest virtual memory test build failed' }
    & analysis/instruction_tests/test_virtual_memory.exe
    if($LASTEXITCODE) { throw 'Guest virtual memory tests failed' }
    python tools/tests/test_physical_memory.py
    if($LASTEXITCODE) { throw 'Physical alias/instruction tests failed' }
    clang++ -std=c++17 -O1 -Wno-macro-redefined -DSIMDE_ENABLE_NATIVE_ALIASES -Itools/tests/fake_nx -Iruntime/switch -Igenerated/ppc -Itools/XenonRecomp/thirdparty/simde tools/tests/test_nx_guest_memory.cpp -o analysis/instruction_tests/test_nx_guest_memory.exe
    if($LASTEXITCODE) { throw 'Switch guest-memory contract test build failed' }
    & analysis/instruction_tests/test_nx_guest_memory.exe
    if($LASTEXITCODE) { throw 'Switch guest-memory contract tests failed' }
    python tools/tests/test_switch_result_format.py
    if($LASTEXITCODE) { throw 'Switch log protocol tests failed' }
    python tools/tests/test_scalar_translation.py
    if($LASTEXITCODE) { throw 'Scalar translation tests failed' }
    python tools/tests/test_conversion_translation.py
    if($LASTEXITCODE) { throw 'Conversion translation tests failed' }
    python tools/tests/test_frsqrte_translation.py
    if($LASTEXITCODE) { throw 'FRSQRTE translation tests failed' }
    python tools/tests/test_game_leaf.py
    if($LASTEXITCODE) { throw 'Real game leaf test failed' }
    aarch64-w64-mingw32-clang++ -std=c++17 -c -Werror=asm-operand-widths -DSIMDE_ENABLE_NATIVE_ALIASES -Igenerated/ppc -Itools/XenonRecomp/thirdparty/simde tools/tests/test_arm64_header.cpp -o analysis/instruction_tests/arm64_state.obj
    if($LASTEXITCODE) { throw 'ARM64 header check failed' }
    clang++ -std=c++17 -O2 -Itools/XenonRecomp/XenonAnalyse tools/tests/test_function_analysis.cpp tools/XenonRecomp/build/XenonAnalyse/libLibXenonAnalyse.a tools/XenonRecomp/build/XenonUtils/libXenonUtils.a tools/XenonRecomp/build/thirdparty/disasm/libdisasm.a -o analysis/instruction_tests/test_function_analysis.exe
    if($LASTEXITCODE) { throw 'Function test build failed' }
    & analysis/instruction_tests/test_function_analysis.exe
    if($LASTEXITCODE) { throw 'Function analysis tests failed' }
    if($CheckGenerated) {
        python tools/check_generated.py
        if($LASTEXITCODE) { throw 'Generated C++ check failed' }
    }
    python tools/summarize_recompile.py *> analysis/test_summary.log
    if($LASTEXITCODE) { throw 'Summary generation failed' }
} finally {
    $env:PATH=$previousPath
    Pop-Location
}
