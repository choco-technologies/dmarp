set(DMOD_MODULE_NAME        test_dmarp)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmarp_test.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(${DMOD_MODULE_NAME} dmarp_if)

# dmnetif: fixture interfaces are registered directly against it (same
#          "/dev/null"-backed pattern as dmnetif's own tests).
# dmroute: dmroute_addr_t, used directly to build test addresses.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmnetif
    dmroute
)
