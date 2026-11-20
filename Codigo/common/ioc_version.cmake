# Versão e commit do firmware dos sensores Pico, a partir do arquivo VERSION (a única
# fonte da versão do projeto) e do git. Os valores saem no boot e no comando VERSION
# (ioc_version.h).
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../common/ioc_version.cmake)
#   ioc_version_definitions(<alvo>)
get_filename_component(IOC_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

function(ioc_version_definitions target)
    set(version "0.0.0-dev")
    if(EXISTS "${IOC_REPO_ROOT}/VERSION")
        file(STRINGS "${IOC_REPO_ROOT}/VERSION" version LIMIT_COUNT 1)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${IOC_REPO_ROOT}/VERSION")
    endif()
    set(commit "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${IOC_REPO_ROOT} rev-parse --short=8 HEAD
                        OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE status)
        if(status EQUAL 0 AND NOT "${head}" STREQUAL "")
            set(commit "${head}")
            execute_process(COMMAND ${GIT_EXECUTABLE} -C ${IOC_REPO_ROOT} status --porcelain --untracked-files=no
                            OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(NOT "${dirty}" STREQUAL "")
                set(commit "${commit}-dirty")
            endif()
        endif()
    endif()
    message(STATUS "iot-over-can: firmware ${version} commit ${commit}")
    target_compile_definitions(${target} PRIVATE IOT_FW_VERSION="${version}" IOT_FW_COMMIT="${commit}")
endfunction()
