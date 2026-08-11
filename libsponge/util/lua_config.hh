#ifndef SPONGE_LIBSPONGE_UTIL_LUA_CONFIG_HH
#define SPONGE_LIBSPONGE_UTIL_LUA_CONFIG_HH

#include <functional>
#include <optional>
#include <string>

// Lua 5.3+ C API. The lua.hpp wrapper externs-C the lua.h/lauxlib.h includes.
#include <lua.hpp>

//! \brief A thin RAII wrapper around a lua_State plus a set of static helpers
//! for reading nested Lua tables with the raw Lua C API.
//!
//! This is intentionally minimal — no sol2/LuaBridge. It exists so that
//! `apps/network_simulator` can load a declarative network topology from a
//! `.lua` file without dragging in a third-party binding library.
//!
//! Typical usage:
//! \code
//!   LuaConfig cfg("etc/network.lua");   // runs the script; result table on top
//!   lua_State *L = cfg.state();
//!   LuaConfig::push_field(L, "interfaces");      // push interfaces table
//!   LuaConfig::for_each(L, [&](int i) {          // iterate array part
//!       LuaConfig::push_index(L, i);
//!       auto name = LuaConfig::get_string(L, "name");
//!       auto ip   = LuaConfig::get_string(L, "ip");
//!       lua_pop(L, 1);                            // pop the element table
//!   });
//!   lua_pop(L, 1);                               // pop interfaces table
//! \endcode
class LuaConfig {
  private:
    lua_State *_L;
    bool _owns_state;

  public:
    //! \brief Construct by loading and running `filename`. The value returned
    //! by the script (the topology table) is left on top of the Lua stack.
    //! Throws std::runtime_error on any load or runtime error.
    explicit LuaConfig(const std::string &filename);

    //! Construct from an existing lua_State (does not take ownership).
    explicit LuaConfig(lua_State *L) : _L(L), _owns_state(false) {}

    ~LuaConfig();

    lua_State *state() { return _L; }

    // --- Static table-reading helpers ---
    // All helpers assume the table to read is on top of the stack unless an
    // explicit index is given. They do NOT pop the table they read from.

    //! \brief Push table[string_key] onto the stack. Returns true if the key
    //! exists (and pushes it regardless of type); false if absent.
    static bool push_field(lua_State *L, const char *key);

    //! \brief Push table[int_index] onto the stack. Returns true if present.
    static bool push_index(lua_State *L, int index);

    //! \brief Read table[key] as an int, without disturbing the stack.
    static std::optional<int> get_int(lua_State *L, const char *key);

    //! \brief Read table[key] as a string, without disturbing the stack.
    static std::optional<std::string> get_string(lua_State *L, const char *key);

    //! \brief Iterate the array part of the table on top of the stack.
    //! For each integer index 1..N, pushes the element and calls `cb(i)`.
    //! The callback is responsible for popping its element (or leaving the
    //! stack balanced — `for_each` restores the stack depth after each call).
    static void for_each(lua_State *L, const std::function<void(int)> &cb);

    //! \brief Iterate the hash part of the table on top of the stack.
    //! For each key, pushes value and calls `cb(key)`. Stack is balanced per
    //! iteration.
    static void for_each_pair(lua_State *L, const std::function<void(const char *key)> &cb);

    //! \brief Return the array length (#t) of the table on top of the stack.
    static size_t array_length(lua_State *L);

    //! \brief Return the string at stack index `idx`, or empty if not a string.
    static std::string peek_string(lua_State *L, int idx);
};

#endif  // SPONGE_LIBSPONGE_UTIL_LUA_CONFIG_HH
