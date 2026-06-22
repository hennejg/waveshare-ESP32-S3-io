idf_component_register(
    SRCS         "scripting.c" "bindings.c"
    INCLUDE_DIRS "include" "."
    REQUIRES     quickjs
)
