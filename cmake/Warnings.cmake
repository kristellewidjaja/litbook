# These are the project warning flags, applied per target and never to third-party code.
# A target opts in by linking against litbook::warnings.

add_library(litbook_warnings INTERFACE)
add_library(litbook::warnings ALIAS litbook_warnings)

target_compile_options(litbook_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wshadow               # a local variable hides another one
    -Wconversion           # implicit narrowing, such as uint64 to uint32
    -Wsign-conversion      # implicit signed and unsigned mixes, critical with unsigned quantities
    -Wnon-virtual-dtor     # a polymorphic class lacks a virtual destructor
    -Wold-style-cast       # C-style casts hide intent
    -Woverloaded-virtual   # a derived function hides a base virtual
)
