cmake_minimum_required(VERSION 3.16)

option(ONDA_INTEGRATION_BUILD
       "Allow the local station-search test endpoint for integration builds"
       OFF)

get_cmake_property(_onda_cmake_variables VARIABLES)
foreach(_onda_variable IN LISTS _onda_cmake_variables)
    if(_onda_variable MATCHES "^CMAKE_(C|CXX)_FLAGS($|_)")
        if("${${_onda_variable}}" MATCHES
           "(-D|/D)[ \t]*(RADIO_INTEGRATION_TEST_HOOKS|RADIO_UI_TEST_FAULT_INJECTION)(=[^ \t;]+)?")
            message(FATAL_ERROR
                "Product firmware cannot define Unity-only fault-injection hooks. "
                "Build those cases with their dedicated Unity test project.")
        endif()
    endif()
endforeach()

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(DEFINED ENV{ONDA_RELEASE_VERSION} AND
   NOT "$ENV{ONDA_RELEASE_VERSION}" STREQUAL "")
    project(radio_portatil VERSION "$ENV{ONDA_RELEASE_VERSION}")
else()
    project(radio_portatil)
endif()

if(CONFIG_ONDA_STATION_SEARCH_TEST_ENDPOINT AND NOT ONDA_INTEGRATION_BUILD)
    message(FATAL_ERROR
        "CONFIG_ONDA_STATION_SEARCH_TEST_ENDPOINT is integration-only. "
        "Use -DONDA_INTEGRATION_BUILD=ON for a local integration image.")
endif()

if(ONDA_INTEGRATION_BUILD AND NOT CONFIG_ONDA_STATION_SEARCH_TEST_ENDPOINT)
    message(FATAL_ERROR
        "ONDA_INTEGRATION_BUILD requires CONFIG_ONDA_STATION_SEARCH_TEST_ENDPOINT=y.")
endif()
