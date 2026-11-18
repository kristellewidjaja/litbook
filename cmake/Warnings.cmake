# Project warning flags, applied per target (never to third-party code).
# Link a target against litbook::warnings to opt in.

add_library(litbook_warnings INTERFACE)
add_library(litbook::warnings ALIAS litbook_warnings)

target_compile_options(litbook_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow               # a local variable hides another one
    -Wconversion           # implicit narrowing, e.g. uint64 -> uint32
    -Wsign-conversion      # implicit signed <-> unsigned, critical with unsigned quantities
    -Wnon-virtual-dtor     # polymorphic class without a virtual destructor
    -Wold-style-cast       # C-style casts hide intent
    -Woverloaded-virtual   # a derived function hides a base virtual
)
