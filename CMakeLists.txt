cmake_minimum_required(VERSION 3.24)
project(mxl-webrtc-monitor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MWM_BUILD_TESTS "Build unit tests" ON)
option(MWM_WITH_NMOS "Link Sony nmos-cpp" OFF)
set(NMOS_CPP_DIR "" CACHE PATH "Path to sony/nmos-cpp Development/")

find_package(Threads REQUIRED)

add_library(mwm-core STATIC
    src/util/sha1.cpp
    src/util/uuid.cpp
    src/util/logging.cpp
    src/util/net.cpp
    src/util/httpclient.cpp
    src/config/config.cpp
    src/config/store.cpp
    src/domain/scan.cpp
    src/channel/state.cpp
    src/channel/audio.cpp
    src/channel/book.cpp
    src/nmos/ids.cpp
    src/nmos/is05.cpp
    src/nmos/connections.cpp
    src/ops/metrics.cpp
    src/ops/httpserver.cpp
    src/ops/mediamtx.cpp
    src/ops/api.cpp
    src/media/v210.cpp
)
target_include_directories(mwm-core PUBLIC src third_party)
target_link_libraries(mwm-core PUBLIC Threads::Threads)
target_compile_options(mwm-core PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(MWM_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_config.cpp
        tests/unit/test_ids.cpp
        tests/unit/test_is05.cpp
        tests/unit/test_state.cpp
        tests/unit/test_domain.cpp
        tests/unit/test_audio.cpp
        tests/unit/test_playback.cpp
        tests/unit/test_platform.cpp
    )
    target_include_directories(unit-tests PRIVATE src third_party)
    target_link_libraries(unit-tests PRIVATE mwm-core)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

find_package(mxl CONFIG)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(GST IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
endif()

if(mxl_FOUND AND GST_FOUND)
    add_library(mwm-media STATIC src/media/engine.cpp)
    target_include_directories(mwm-media PUBLIC src third_party)
    target_link_libraries(mwm-media PUBLIC mwm-core mxl::mxl PkgConfig::GST)
    target_compile_options(mwm-media PRIVATE -Wall -Wextra -Wno-unused-parameter)

    option(MWM_USE_PREBUILT_WEBUI "Embed web/dist/index.html instead of running npm" OFF)
    set(WEBUI_SOURCES
        web/index.html
        web/package.json
        web/package-lock.json
        web/vite.config.js
        web/src/main.js
        web/src/App.vue
        web/src/style.css
    )
    set(WEBUI_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/ops/webui_generated.hpp)
    find_program(NPM_EXECUTABLE npm)
    if(MWM_USE_PREBUILT_WEBUI)
        set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html)
        if(NOT EXISTS ${WEBUI_HTML})
            message(FATAL_ERROR "MWM_USE_PREBUILT_WEBUI=ON but ${WEBUI_HTML} is missing")
        endif()
        add_custom_command(
            OUTPUT ${WEBUI_GENERATED}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED} -DVAR=indexHtml
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
            COMMENT "Embedding prebuilt web UI"
        )
        set(MWM_UI_SOURCE ${WEBUI_GENERATED})
    elseif(NPM_EXECUTABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/web/package-lock.json)
        set(WEBUI_HTML ${CMAKE_CURRENT_BINARY_DIR}/webui/index.html)
        add_custom_command(
            OUTPUT ${WEBUI_HTML}
            COMMAND ${NPM_EXECUTABLE} ci --no-fund --no-audit
            COMMAND ${NPM_EXECUTABLE} run build
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/webui
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html ${WEBUI_HTML}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
            DEPENDS ${WEBUI_SOURCES}
            COMMENT "Building Vue web UI"
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WEBUI_GENERATED}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED} -DVAR=indexHtml
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
            COMMENT "Embedding web UI"
        )
        set(MWM_UI_SOURCE ${WEBUI_GENERATED})
    else()
        set(MWM_UI_SOURCE "")
    endif()

    add_executable(mxl-webrtc-monitor src/main.cpp ${MWM_UI_SOURCE})
    target_include_directories(mxl-webrtc-monitor PRIVATE src third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(mxl-webrtc-monitor PRIVATE mwm-media)
    target_compile_options(mxl-webrtc-monitor PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(MWM_UI_SOURCE)
        target_compile_definitions(mxl-webrtc-monitor PRIVATE MWM_HAS_UI=1)
    endif()

    add_executable(mxl-flow-writer tests/tools/flow_writer.cpp)
    target_link_libraries(mxl-flow-writer PRIVATE mxl::mxl mwm-core)
    target_compile_features(mxl-flow-writer PRIVATE cxx_std_20)

    if(MWM_WITH_NMOS)
        if(NOT NMOS_CPP_DIR)
            message(FATAL_ERROR "MWM_WITH_NMOS=ON requires -DNMOS_CPP_DIR")
        endif()
        set(NMOS_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(NMOS_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_SUPPLIED_JSON_SCHEMA_VALIDATOR ON CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_SUPPLIED_JWT_CPP ON CACHE BOOL "" FORCE)
        set(CMAKE_CXX_STANDARD 17)
        add_subdirectory(${NMOS_CPP_DIR} ${CMAKE_BINARY_DIR}/nmos-cpp EXCLUDE_FROM_ALL)
        set(CMAKE_CXX_STANDARD 20)
        target_sources(mxl-webrtc-monitor PRIVATE src/nmos/node.cpp)
        target_compile_definitions(mxl-webrtc-monitor PRIVATE MWM_WITH_NMOS=1)
        target_link_libraries(mxl-webrtc-monitor PRIVATE nmos-cpp::nmos-cpp)
        set_source_files_properties(src/nmos/node.cpp PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            COMPILE_OPTIONS "-w")
    else()
        target_sources(mxl-webrtc-monitor PRIVATE src/nmos/node.cpp)
    endif()

    install(TARGETS mxl-webrtc-monitor mxl-flow-writer RUNTIME DESTINATION bin)
else()
    message(STATUS "mxl or gstreamer not found; building unit tests only")
endif()
