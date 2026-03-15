add_executable(CTAnalyzerX WIN32)

target_sources(CTAnalyzerX
  PRIVATE
    src/main.cpp
    src/MainWindow.cpp
    src/WorkflowPanelWidget.cpp
    src/WorkflowStateMachine.cpp
    resources/MainWindow.ui
    resources/WorkflowPanelWidget.ui
  PUBLIC
    include/MainWindow.h
    include/WorkflowPanelWidget.h
    include/WorkflowStateMachine.h
)

target_include_directories(CTAnalyzerX
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(CTAnalyzerX
  PRIVATE
    CTAXGui
)

# Ensure uic sees the app's .ui files AND any ui_*.h included from linked libs (e.g., CTAXGui)
set_property(TARGET CTAnalyzerX PROPERTY AUTOUIC_SEARCH_PATHS
  ${CMAKE_CURRENT_SOURCE_DIR}/resources
  ${CMAKE_CURRENT_SOURCE_DIR}/../lib/gui/resources
)

# Suppress linker duplicate symbol errors
if(MSVC)
    target_link_options(CTAnalyzerX PRIVATE 
        /FORCE:MULTIPLE
        /IGNORE:4006  # Ignore duplicate symbol warnings
    )
    
    # Suppress STL deprecation warnings
    target_compile_definitions(CTAnalyzerX PRIVATE
        _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
        _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS
    )
endif()