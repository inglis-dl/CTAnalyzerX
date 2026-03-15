# Add tools here.
option(CTAX_BUILD_TOOL_PROTOTYPE "Build CTAXPrototype tool" OFF)
option(CTAX_BUILD_TOOL_VIEWER "Build CTAXViewer tool" OFF)

if(CTAX_BUILD_TOOL_PROTOTYPE)
  add_subdirectory(Prototype)
endif()

if(CTAX_BUILD_TOOL_VIEWER)
  add_subdirectory(Viewer)
endif()