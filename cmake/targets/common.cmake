# common target definitions
# this file will also load platform specific macros

if(APPLE AND NOT SUNSHINE_BUILD_HOMEBREW)
    add_executable(sunshine MACOSX_BUNDLE ${SUNSHINE_TARGET_FILES})
else()
    add_executable(sunshine ${SUNSHINE_TARGET_FILES})
endif()
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(sunshine ${dep})  # compile these before sunshine
endforeach()

# platform specific target definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/targets/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/targets/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/targets/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/targets/linux.cmake)
    endif()
endif()

target_link_libraries(sunshine ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
target_compile_definitions(sunshine PUBLIC ${SUNSHINE_DEFINITIONS})

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS SUNSHINE_COMPILE_OPTIONS)
        list(APPEND SUNSHINE_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(sunshine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(sunshine PRIVATE ${SUNSHINE_LINK_OPTIONS})

if(SUNSHINE_BUILD_EMBEDDED)
    if(NOT WIN32)
        message(FATAL_ERROR "The embedded Sunshine target currently supports Windows only.")
    endif()
    add_library(sunshine_embedded SHARED ${SUNSHINE_TARGET_FILES})
    set_target_properties(sunshine_embedded PROPERTIES PREFIX "" OUTPUT_NAME "soluna-host")
    foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
        add_dependencies(sunshine_embedded ${dep})
    endforeach()
    set(_embedded_libraries ${SUNSHINE_EXTERNAL_LIBRARIES})
    list(REMOVE_ITEM _embedded_libraries "$<TARGET_OBJECTS:sunshine_rc_object>")
    target_link_libraries(sunshine_embedded PUBLIC ${_embedded_libraries} ${EXTRA_LIBS})
    target_compile_definitions(sunshine_embedded PRIVATE ${SUNSHINE_DEFINITIONS} SUNSHINE_EMBEDDED)
    target_compile_options(sunshine_embedded PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    target_link_options(sunshine_embedded PRIVATE -static-libgcc -static-libstdc++ -Wl,-s)
endif()

# docs
if(BUILD_DOCS)
    include(third-party/dockle/cmake/Dockle.cmake)
    dockle_add_docs(docs TARGETS docs)
endif()

# tests
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# custom compile flags, must be after adding tests

if (NOT BUILD_TESTS)
    set(TEST_DIR "")
else()
    set(TEST_DIR "${CMAKE_SOURCE_DIR}/tests")
endif()

# src/upnp
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS -Wno-pedantic)

# src/nvhttp
string(TOUPPER "x${CMAKE_BUILD_TYPE}" BUILD_TYPE)
if("${BUILD_TYPE}" STREQUAL "XDEBUG")
    if(WIN32)
        if (NOT BUILD_TESTS)
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}"
                    PROPERTIES COMPILE_FLAGS -O2)
        else()
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests"
                    PROPERTIES COMPILE_FLAGS -O2)
        endif()
    endif()
else()
    add_definitions(-DNDEBUG)
endif()
