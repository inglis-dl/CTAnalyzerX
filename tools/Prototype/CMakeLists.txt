add_executable(CTAXPrototype)

target_sources(CTAXPrototype
  PRIVATE
    src/main.cpp
)

target_link_libraries(CTAXPrototype
  PRIVATE
    CTAXCore
)