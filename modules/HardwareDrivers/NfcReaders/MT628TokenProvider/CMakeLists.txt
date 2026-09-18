#
# AUTO GENERATED - MARKED REGIONS WILL BE KEPT
# template version 3
#

# module setup:
#   - ${MODULE_NAME}: module name
ev_setup_cpp_module()

# ev@bcc62523-e22b-41d7-ba2f-825b493a3c97:v1

add_library(mt628_serial STATIC
    mt628_serial/MT628Serial.cpp
)

target_include_directories(mt628_serial
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/mt628_serial
)

target_include_directories(${MODULE_NAME}
    PRIVATE
        mt628_serial
)

target_link_libraries(${MODULE_NAME}
    PRIVATE
        everest::log
        everest::helpers
        mt628_serial
)

# ev@bcc62523-e22b-41d7-ba2f-825b493a3c97:v1

target_sources(${MODULE_NAME}
    PRIVATE
        "main/auth_token_providerImpl.cpp"
)

# ev@c55432ab-152c-45a9-9d2e-7281d50c69c3:v1
# insert other things like install cmds etc here
# ev@c55432ab-152c-45a9-9d2e-7281d50c69c3:v1
