// emacs-rpc-bridge — a C++20 native module for Emacs.
//
// This file is intentionally small but shows every piece a real module needs:
//   1. the GPL-compatibility symbol Emacs refuses to load modules without,
//   2. the emacs_module_init entry point Emacs calls at module-load time,
//   3. the callback signature shared by every exported function, and
//   4. the string round-trip helpers (returning strings to Emacs and reading
//      string arguments from Emacs).
//
// Build with `mise build` (CMake, see CMakeLists.txt).  The result,
// build/libemacs-rpc-bridge.so, is a plain shared library: you load it with
// module-load and wrap it however you like — nothing here depends on a
// particular Emacs setup.

#include <emacs-module.h>  // extern "C" linkage is applied by this header

#include <algorithm>
#include <cctype>
#include <string>

// Emacs only loads modules that declare themselves GPL-compatible.  Presence
// of the symbol is what matters, not its value; the initializer turns this
// into a definition (a bare `extern "C" int x;' declaration would emit no
// symbol at all).  The extern "C" block gives the variable C linkage without
// an extern storage-class specifier, keeping the plain C name instead of a
// mangled C++ one.
extern "C" {
int plugin_is_GPL_compatible = 1;
}

namespace {

// Lisp symbol prefix under which the functions are registered: the module
// exports emacs-rpc-bridge-hello, never a bare "hello" that could collide
// with another package.
const std::string kSymbolPrefix = "emacs-rpc-bridge-";

// ---- string helpers --------------------------------------------------------
//
// Text crosses the module boundary as UTF-8.  These two helpers cover both
// directions and are what almost every real function ends up using; numbers
// and vectors follow the analogous make_* / *_contents calls of the API.

emacs_value make_emacs_string(emacs_env *env, const std::string &s) {
  return env->make_string(env, s.data(), static_cast<ptrdiff_t>(s.size()));
}

std::string emacs_string_arg(emacs_env *env, emacs_value value) {
  // First call with a null buffer asks for the size in bytes, counting the
  // trailing NUL that copy_string_contents always writes.
  ptrdiff_t size = 0;
  if (!env->copy_string_contents(env, value, nullptr, &size))
    return {};  // VALUE is not a string; the API signals wrong-type-argument
  std::string s(static_cast<std::size_t>(size) - 1, '\0');
  env->copy_string_contents(env, value, s.data(), &size);
  return s;
}

// ---- exported functions ----------------------------------------------------
//
// Every function exported by a module has this exact callback shape: the
// environment, the number of arguments actually passed, the argument vector,
// and user data from make_function.  When compiled as C++17 or newer the API
// header types these callbacks `noexcept' ("must not throw C++ exceptions"),
// hence the noexcept below; a module callback must never let an exception
// escape into Emacs.  The callbacks are only reachable through
// emacs_module_init below, so they live in this anonymous namespace
// (internal linkage) instead of polluting the .so's symbol table.

emacs_value F_hello(emacs_env *env, ptrdiff_t nargs,
                    emacs_value args[], void *data) noexcept {
  (void)nargs; (void)args; (void)data;
  return make_emacs_string(env, "Hello from a C++20 Emacs module");
}

emacs_value F_shout(emacs_env *env, ptrdiff_t nargs,
                    emacs_value args[], void *data) noexcept {
  (void)nargs; (void)data;
  std::string s = emacs_string_arg(env, args[0]);
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return make_emacs_string(env, s);
}

// ---- registration ----------------------------------------------------------
//
// A module has no dedicated registration table: functions are added by
// calling ordinary Lisp, exactly as in the sample module of the Emacs Lisp
// manual.  defalias is invoked as
//
//     (defalias 'emacs-rpc-bridge-hello <function-object>)
//
// so the callback becomes a first-class function the user can call, advise,
// and wrap in interactive commands.  The docstring is attached through
// make_function at the call site below.

void defun(emacs_env *env, const std::string &suffix, emacs_value fn) {
  emacs_value Qdefalias = env->intern(env, "defalias");
  emacs_value symbol = env->intern(env, (kSymbolPrefix + suffix).c_str());
  emacs_value args[] = {symbol, fn};
  env->funcall(env, Qdefalias, 2, args);
}

}  // namespace

// ---- module entry point ----------------------------------------------------
//
// The only symbol Emacs looks up in the .so (besides
// plugin_is_GPL_compatible).  Emacs calls it once when module-load succeeds;
// returning nonzero aborts the load.

extern "C" int emacs_module_init(struct emacs_runtime *ert) {
  emacs_env *env = ert->get_environment(ert);

  // make_function(env, min-args, max-args, callback, docstring, user-data)
  // fixes the callback's arity; defun then binds it to a Lisp symbol.
  defun(env, "hello",
        env->make_function(env, 0, 0, F_hello,
                           "Return a greeting string from the module.",
                           nullptr));
  defun(env, "shout",
        env->make_function(env, 1, 1, F_shout,
                           "Return ARG converted to upper case.",
                           nullptr));

  return 0;
}
