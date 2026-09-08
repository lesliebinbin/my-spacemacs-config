cmake_minimum_required(VERSION 3.16)

project(emacs-rpc-bridge VERSION 0.1.0 LANGUAGES CXX)

# C++20 is a hard requirement; do not silently fall back to extensions.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# emacs-module.h is installed alongside Emacs itself, so its location depends
# on how Emacs was installed (source build prefix, distro packaging, ...).
# The header must match the Emacs that will load the module.
find_path(EMACS_MODULE_INCLUDE_DIR NAMES emacs-module.h
  HINTS "$ENV{EMACS_MODULE_INCLUDE_DIR}"
        "/usr/local/include" "/usr/include")
if(NOT EMACS_MODULE_INCLUDE_DIR)
  message(FATAL_ERROR
    "emacs-module.h not found.  If your Emacs installs its headers in a "
    "nonstandard place, pass -DEMACS_MODULE_INCLUDE_DIR=/path/to/include.")
endif()

# MODULE (not SHARED): Emacs dlopens the built file directly, so the library
# needs no link dependency of its own (per runbook/emacs-rpc-bridge/design-001).
add_library(emacs-rpc-bridge MODULE src/module.cpp)
target_include_directories(emacs-rpc-bridge PRIVATE "${EMACS_MODULE_INCLUDE_DIR}")
target_compile_options(emacs-rpc-bridge PRIVATE -Wall -Wextra)

# Emacs has no install convention of its own.  Either load the built file
# directly (build/libemacs-rpc-bridge.so) with module-load, or install it
# wherever your Emacs setup keeps extension libraries:
#
#   cmake --install build --prefix /your/extension/lib-dir
#
# which places it under <prefix>/emacs-rpc-bridge/.
install(TARGETS emacs-rpc-bridge
        LIBRARY DESTINATION "emacs-rpc-bridge")
