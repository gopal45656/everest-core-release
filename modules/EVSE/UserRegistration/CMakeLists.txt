ev_setup_cpp_module()

target_link_libraries(${MODULE_NAME}
    PRIVATE
        everest::log
)

target_sources(${MODULE_NAME}
    PRIVATE
    "main/emptyImpl.cpp"
)