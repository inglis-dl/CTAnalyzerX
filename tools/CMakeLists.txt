# Add tools here.
option(CTAX_BUILD_TOOL_PROTOTYPE "Build CTAXPrototype tool" OFF)
option(CTAX_BUILD_TOOL_VIEWER "Build CTAXViewer tool" OFF)
option(CTAX_BUILD_TOOL_BATCH "Build CTAXBatchProcessor tool" OFF)
option(CTAX_BUILD_TOOL_MORPHOMETRY "Build CTAXMorphometry tool" OFF)

if(CTAX_BUILD_TOOL_PROTOTYPE)
  add_subdirectory(Prototype)
endif()

if(CTAX_BUILD_TOOL_VIEWER)
  add_subdirectory(Viewer)
endif()

if(CTAX_BUILD_TOOL_BATCH)
  add_subdirectory(BatchProcessor)
endif()

if(CTAX_BUILD_TOOL_MORPHOMETRY)
  add_subdirectory(Morphometry)
endif()