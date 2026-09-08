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

# ---------------------------------------------------------------------------
# Native module (milestone 1: scaffold and validate the CMake build)
# ---------------------------------------------------------------------------
# MODULE (not SHARED): Emacs dlopens the built file directly, so the library
# needs no link dependency of its own (per runbook/emacs-rpc-bridge/design-001).
find_package(Threads REQUIRED)

add_library(emacs-rpc-bridge MODULE
  src/module.cpp
  src/server.cpp
  src/dispatch.cpp
  src/handles.cpp)
target_include_directories(emacs-rpc-bridge PRIVATE "${EMACS_MODULE_INCLUDE_DIR}")
target_compile_options(emacs-rpc-bridge PRIVATE -Wall -Wextra)
# gRPC / protobuf / absl are static (installed into $HOME/local), so this
# dlopened MODULE pulls them in and must link position-independently.
target_link_libraries(emacs-rpc-bridge PRIVATE rpc_bridge_proto
                      Threads::Threads)

# Emacs has no install convention of its own.  Either load the built file
# directly (build/libemacs-rpc-bridge.so) with module-load, or install it
# wherever your Emacs setup keeps extension libraries:
#
#   cmake --install build --prefix /your/extension/lib-dir
#
# which places it under <prefix>/emacs-rpc-bridge/.
install(TARGETS emacs-rpc-bridge
        LIBRARY DESTINATION "emacs-rpc-bridge")

# ---------------------------------------------------------------------------
# Protocol bindings + codegen smoke (milestone 2: define the protocol
# contract, generate bindings, validate the toolchain).
#
# Toolchain: gRPC v1.83.1 (protobuf 35.1, boringssl module provider) built
# from source into /home/lesliebinbinhuang/local; that prefix is supplied to
# find_package via CMAKE_PREFIX_PATH in CMakePresets.json.  If configure
# fails to find gRPC here, re-check the preset path before installing
# anything else — no silent dependency downloads.
# ---------------------------------------------------------------------------
# protobuf FIRST: gRPC's exported targets reference protobuf::libprotobuf and
# protobuf::libprotoc, so protobuf's config must be loaded before gRPC's.
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
# protoc does not create its own output directory.
file(MAKE_DIRECTORY "${GEN_DIR}")

add_custom_command(
  OUTPUT "${GEN_DIR}/emacs_rpc.pb.cc" "${GEN_DIR}/emacs_rpc.pb.h"
         "${GEN_DIR}/emacs_rpc.grpc.pb.cc" "${GEN_DIR}/emacs_rpc.grpc.pb.h"
  COMMAND $<TARGET_FILE:protobuf::protoc>
          -I "${PROTO_ROOT}"
          --cpp_out "${GEN_DIR}"
          "${PROTO_ROOT}/emacs_rpc.proto"
  COMMAND $<TARGET_FILE:protobuf::protoc>
          -I "${PROTO_ROOT}"
          --grpc_out "${GEN_DIR}"
          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
          "${PROTO_ROOT}/emacs_rpc.proto"
  DEPENDS "${PROTO_ROOT}/emacs_rpc.proto"
  VERBATIM
)

# Generated code only — a hand edit here is overwritten on the next build.
# PIC: this static library is linked into the dlopened MODULE library
# (emacs-rpc-bridge) below.
add_library(rpc_bridge_proto STATIC
  "${GEN_DIR}/emacs_rpc.pb.cc"
  "${GEN_DIR}/emacs_rpc.grpc.pb.cc")
set_target_properties(rpc_bridge_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(rpc_bridge_proto PUBLIC "${GEN_DIR}")
target_link_libraries(rpc_bridge_proto PUBLIC gRPC::grpc++)

# Standalone smoke executable: proves the generated bindings compile and the
# wire messages round-trip, with no Emacs or bridge logic involved.
add_executable(codegen_smoke tests/codegen_smoke.cpp)
target_link_libraries(codegen_smoke PRIVATE rpc_bridge_proto)

enable_testing()
add_test(NAME codegen_smoke COMMAND codegen_smoke)

# ---------------------------------------------------------------------------
# Integration harness (TODO 3 acceptance): end-to-end dispatch + object
# lifetime over the real Unix socket.  The test spawns its own batch Emacs
# (tests/emacs_bridge_server.el) and needs an `emacs' on PATH (override with
# ERB_EMACS).  The 120 s ctest timeout plus per-RPC deadlines turn any
# deadlock into a failed test, never a hung one.
# ---------------------------------------------------------------------------
add_executable(integration_test tests/integration_test.cpp)
target_compile_definitions(integration_test PRIVATE
  RPC_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(integration_test PRIVATE rpc_bridge_proto)
add_test(NAME integration_test COMMAND integration_test)
set_tests_properties(integration_test PROPERTIES TIMEOUT 120)
