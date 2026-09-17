idf_component_register(
    SRCS
        "src/cap_ssh.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
        json
        wolfssl
        wolfssh
        mbedtls
        lwip
        freertos
)

# The wolfssl component sets this via CMAKE_C_FLAGS in its own CMakeLists.txt,
# which ESP-IDF's per-component compile-options list does not pick up for
# other components — set it explicitly here so wolfssh/ssh.h takes its
# "#include <wolfssl/wolfcrypt/settings.h>" branch instead of the plain
# "#include <wolfssl/options.h>" branch (which this managed setup never
# generates).
target_compile_definitions(${COMPONENT_LIB} PRIVATE WOLFSSL_USER_SETTINGS)
