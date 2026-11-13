cmake_minimum_required(VERSION 3.22)

project(EvoViewerSetupPlugin LANGUAGES CXX)

find_package(Qt6 COMPONENTS Widgets Network WebEngineWidgets REQUIRED)
find_package(ManiVault COMPONENTS Core PointData ClusterData CONFIG QUIET)
mv_project_defaults()

set(PLUGIN_SOURCES
    src/EvoViewerSetupPlugin.h
    src/EvoViewerSetupPlugin.cpp
    src/EvoViewerSetupRequirements.h
    src/EvoViewerSetupRequirements.cpp
    src/EvoViewerSetupData.h
    src/EvoViewerSetupData.cpp
    src/EvoViewerSetupWorkflow.cpp
    src/TiledStorageExperiment.cpp
    PluginInfo.json
)

add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCES})

if(NOT TARGET QtTooltipKit::Tooltips)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../qt-tooltip-kit/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../qt-tooltip-kit" "${CMAKE_BINARY_DIR}/qt-tooltip-kit")
    else()
        set(QT_TOOLTIP_KIT_REPOSITORY "https://github.com/sbvis/qt-tooltip-kit.git" CACHE STRING "Qt Tooltip Kit repository")
        set(QT_TOOLTIP_KIT_REVISION "4940bfd0a17358a2b7e17e12d41688c22b9a9615" CACHE STRING "Qt Tooltip Kit commit")
        include(FetchContent)
        FetchContent_Declare(QtTooltipKit
            GIT_REPOSITORY "${QT_TOOLTIP_KIT_REPOSITORY}"
            GIT_TAG "${QT_TOOLTIP_KIT_REVISION}")
        FetchContent_MakeAvailable(QtTooltipKit)
    endif()
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE QtTooltipKit::Tooltips)
target_include_directories(${PROJECT_NAME} PRIVATE "${ManiVault_INCLUDE_DIR}")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets Qt6::Network Qt6::WebEngineWidgets ManiVault::Core ManiVault::PointData ManiVault::ClusterData)
set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON)

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION Plugins COMPONENT PLUGINS
    LIBRARY DESTINATION Plugins COMPONENT PLUGINS
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        --install ${CMAKE_CURRENT_BINARY_DIR}
        --config $<CONFIGURATION>
        --prefix ${ManiVault_INSTALL_DIR}/$<CONFIGURATION>
)

mv_handle_plugin_config(${PROJECT_NAME} 0)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<IF:$<CONFIG:DEBUG>,${ManiVault_INSTALL_DIR}/Debug,$<IF:$<CONFIG:RELWITHDEBINFO>,${ManiVault_INSTALL_DIR}/RelWithDebInfo,${ManiVault_INSTALL_DIR}/Release>>)
    set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_COMMAND $<IF:$<CONFIG:DEBUG>,"${ManiVault_INSTALL_DIR}/Debug/ManiVault Studio.exe",$<IF:$<CONFIG:RELWITHDEBINFO>,"${ManiVault_INSTALL_DIR}/RelWithDebInfo/ManiVault Studio.exe","${ManiVault_INSTALL_DIR}/Release/ManiVault Studio.exe">>)
endif()
