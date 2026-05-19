# SPDX-License-Identifier: MIT
# psynder_script — Lane 15. Owns: Lua 5.4 binding, system/component registration macros, REPL.
psynder_add_lane(script LINKS psynder_core psynder_scene)
if(TARGET lua::lua)
    target_link_libraries(psynder_script PUBLIC lua::lua)
elseif(TARGET Lua::Lua)
    target_link_libraries(psynder_script PUBLIC Lua::Lua)
elseif(Lua_FOUND)
    target_include_directories(psynder_script PUBLIC ${LUA_INCLUDE_DIR})
    target_link_libraries(psynder_script PUBLIC ${LUA_LIBRARIES})
endif()
