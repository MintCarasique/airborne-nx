# Writes OUT (build_id.h) with the git commit of the project and whether the tree had changes, for the
# startup log line (AGENTS.md: hardware logs carry a build ID). Run at every build by the airborne_build_id
# target; the file is rewritten only when the ID changes, so an unchanged tree recompiles nothing.
#   cmake -DROOT=<repo> -DOUT=<file> -P build_id.cmake
execute_process(COMMAND git -C "${ROOT}" rev-parse --short=7 HEAD
                OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE commit_result)
if(NOT commit_result EQUAL 0 OR commit STREQUAL "")
  set(commit "unknown")
endif()
execute_process(COMMAND git -C "${ROOT}" status --porcelain --untracked-files=no
                OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT changes STREQUAL "")
  set(commit "${commit}-dirty")
endif()
set(text "#pragma once\n#define AIRBORNE_BUILD_ID \"${commit}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
else()
  set(old "")
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
