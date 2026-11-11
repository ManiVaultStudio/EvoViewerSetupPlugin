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
