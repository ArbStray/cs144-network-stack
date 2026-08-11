find_library (LIBPCAP pcap)
find_library (LIBPTHREAD pthread)

# Lua: used by the Lua-driven network_simulator topology loader.
# Try Lua 5.4 / 5.3 / generic "lua" in order.
find_library (LIBLUA NAMES lua5.4 lua5.3 lua)
find_path (LUA_INCLUDE_DIR NAMES lua.hpp lua.h
           PATH_SUFFIXES include/lua5.4 include/lua5.3 include/lua include)
if (LIBLUA AND LUA_INCLUDE_DIR)
    message (STATUS "Found Lua: ${LIBLUA} (include: ${LUA_INCLUDE_DIR})")
else()
    message (WARNING "Lua not found; Lua-driven network simulator will not link. "
                     "Install liblua5.4-dev (apt) or equivalent to enable it.")
endif()

macro (add_sponge_exec exec_name)
    add_executable ("${exec_name}" "${exec_name}.cc")
    target_link_libraries ("${exec_name}" ${ARGN} sponge ${LIBPTHREAD})
endmacro (add_sponge_exec)
