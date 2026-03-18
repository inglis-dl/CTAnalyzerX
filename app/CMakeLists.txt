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
  ${CTAX_GUI_RESOURCES_DIR}
)

# VTK autoinit is handled by CTAXCore and CTAXGui, no need to repeat here