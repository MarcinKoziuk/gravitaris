# Renders cmake/build-config.hpp.in into the build tree with the current git
# state and timestamp. Run via `cmake -P` both at configure time (so the
# header exists before the first compile) and as a build step, so the stamp
# tracks the build rather than the last reconfigure.
#
# Inputs: GT_SOURCE_DIR, GT_TEMPLATE, GT_OUTPUT, GT_PLATFORM, GT_BUILD_TYPE.

find_package(Git QUIET)

set(GT_GIT_HASH "unknown")
set(GT_GIT_DIRTY 0)

if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${GT_SOURCE_DIR}"
        OUTPUT_VARIABLE _gtHash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _gtResult
    )
    if(_gtResult EQUAL 0 AND _gtHash)
        set(GT_GIT_HASH "${_gtHash}")
    endif()

    # --untracked-files=no: a scratch file or a fresh build dir isn't a
    # modified build, and would otherwise make every tree read as dirty.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${GT_SOURCE_DIR}"
        OUTPUT_VARIABLE _gtStatus
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _gtResult
    )
    if(_gtResult EQUAL 0 AND _gtStatus)
        set(GT_GIT_DIRTY 1)
    endif()
endif()

if(NOT GT_BUILD_TYPE)
    set(GT_BUILD_TYPE "Unknown")
endif()

string(TIMESTAMP GT_TIMESTAMP "%Y-%m-%d %H:%M" UTC)

configure_file("${GT_TEMPLATE}" "${GT_OUTPUT}" @ONLY)
