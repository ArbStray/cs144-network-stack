#include "lua_config.hh"

#include <stdexcept>

LuaConfig::LuaConfig(const std::string &filename) : _owns_state(true) {
    _L = luaL_newstate();
    if (!_L) {
        throw std::runtime_error("LuaConfig: failed to create lua_State");
    }
    // We deliberately do NOT open the standard libraries — the topology file
    // is pure data (a table literal returned by the script), so the base
    // library is enough and keeps the sandbox minimal. If future scripts need
    // string/math/etc., call luaL_openlibs here.
    luaL_openlibs(_L);

    const int status = luaL_dofile(_L, filename.c_str());
    if (status != LUA_OK) {
        std::string err = "LuaConfig: error loading ";
        err += filename;
        const char *lua_err = lua_tostring(_L, -1);
        if (lua_err) {
            err += ": ";
            err += lua_err;
        }
        lua_close(_L);
        _L = nullptr;
        throw std::runtime_error(err);
    }

    // The script must return a table (the topology). If it returned nothing
    // or a non-table, that's a configuration error.
    if (lua_gettop(_L) == 0 || !lua_istable(_L, -1)) {
        lua_close(_L);
        _L = nullptr;
        throw std::runtime_error("LuaConfig: " + filename + " did not return a table");
    }
}

LuaConfig::~LuaConfig() {
    if (_owns_state && _L) {
        lua_close(_L);
    }
}

bool LuaConfig::push_field(lua_State *L, const char *key) {
    // table is at -1; push table[key] onto stack
    lua_pushstring(L, key);   // stack: table, key
    lua_gettable(L, -2);       // pops key, pushes value; stack: table, value
    const bool present = !lua_isnil(L, -1);
    if (!present) {
        lua_pop(L, 1);  // pop the nil, leave table on top
    }
    return present;
}

bool LuaConfig::push_index(lua_State *L, int index) {
    lua_pushinteger(L, index);  // stack: table, index
    lua_gettable(L, -2);         // stack: table, value
    const bool present = !lua_isnil(L, -1);
    if (!present) {
        lua_pop(L, 1);
    }
    return present;
}

std::optional<int> LuaConfig::get_int(lua_State *L, const char *key) {
    lua_pushstring(L, key);
    lua_gettable(L, -2);  // stack: table, value
    std::optional<int> result;
    if (lua_isinteger(L, -1)) {
        result = static_cast<int>(lua_tointeger(L, -1));
    } else if (lua_isnumber(L, -1)) {
        result = static_cast<int>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);  // pop value, leave table
    return result;
}

std::optional<std::string> LuaConfig::get_string(lua_State *L, const char *key) {
    lua_pushstring(L, key);
    lua_gettable(L, -2);  // stack: table, value
    std::optional<std::string> result;
    if (lua_isstring(L, -1)) {
        result = std::string(lua_tostring(L, -1));
    }
    lua_pop(L, 1);
    return result;
}

void LuaConfig::for_each(lua_State *L, const std::function<void(int)> &cb) {
    // table is at -1. Iterate 1..#t, pushing each element.
    const int base = lua_absindex(L, -1);
    const int n = static_cast<int>(luaL_len(L, base));
    for (int i = 1; i <= n; ++i) {
        lua_geti(L, base, i);  // push table[i]
        const int depth_before = lua_gettop(L);
        cb(i);
        // Restore stack: pop anything the callback left, then the element.
        const int depth_after = lua_gettop(L);
        if (depth_after > depth_before) {
            lua_pop(L, depth_after - depth_before);
        }
        lua_pop(L, 1);  // pop the element
    }
}

void LuaConfig::for_each_pair(lua_State *L, const std::function<void(const char *key)> &cb) {
    // table is at -1. Use lua_next to walk the hash part.
    const int base = lua_absindex(L, -1);
    lua_pushnil(L);  // first key
    while (lua_next(L, base) != 0) {
        // stack: ..., key, value
        const char *key = lua_tostring(L, -2);  // key at -2
        if (key) {
            const int depth_before = lua_gettop(L);
            cb(key);
            const int depth_after = lua_gettop(L);
            if (depth_after > depth_before) {
                lua_pop(L, depth_after - depth_before);
            }
        }
        lua_pop(L, 1);  // pop value, keep key for next iteration
    }
}

size_t LuaConfig::array_length(lua_State *L) {
    return static_cast<size_t>(luaL_len(L, -1));
}

std::string LuaConfig::peek_string(lua_State *L, int idx) {
    if (lua_isstring(L, idx)) {
        return std::string(lua_tostring(L, idx));
    }
    return {};
}
