// SPDX-License-Identifier: MIT
// Psynder — PsyGraph authoring model helpers. Lane 15 owns.

#include "Graph.h"

namespace psynder::script::psygraph {

u32 Graph::intern_string(std::string_view s) {
    for (u32 i = 0; i < static_cast<u32>(strings.size()); ++i) {
        if (strings[i] == s)
            return i;
    }
    strings.emplace_back(s);
    return static_cast<u32>(strings.size() - 1);
}

}  // namespace psynder::script::psygraph
