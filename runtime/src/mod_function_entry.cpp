#include "mod_plugins.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

struct FunctionEntryPlugin {
    std::string id;
    uint32_t address = 0;
    PSXModFunctionEntryCallback callback = nullptr;
};

std::vector<FunctionEntryPlugin>& function_entry_plugins() {
    static std::vector<FunctionEntryPlugin> value;
    return value;
}

/* One callback may dispatch another opted-in guest function synchronously.
 * Preserve the outer decision so an inner replacement cannot accidentally
 * replace its caller, while an outer callback can still request replacement
 * after the nested dispatch returns. */
static thread_local CPUState* s_mod_entry_cpu = nullptr;
static thread_local bool s_mod_entry_skip = false;

}  // namespace

extern "C" int psx_mod_register_function_entry_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback) {
    if (!id || !*id || !address || !callback) return 0;
    auto& plugins = function_entry_plugins();
    const auto duplicate = std::find_if(
        plugins.begin(), plugins.end(), [&](const FunctionEntryPlugin& item) {
            return item.id == id && item.address == address;
        });
    if (duplicate != plugins.end()) return 0;
    plugins.push_back(FunctionEntryPlugin{id, address, callback});
    return 1;
}

extern "C" void psx_mod_skip_current_function(CPUState* cpu) {
    if (cpu && cpu == s_mod_entry_cpu) s_mod_entry_skip = true;
}

extern "C" int psx_mod_function_entry(CPUState* cpu, uint32_t address) {
    if (!cpu) return 0;
    CPUState* const outer_cpu = s_mod_entry_cpu;
    const bool outer_skip = s_mod_entry_skip;
    s_mod_entry_cpu = cpu;
    s_mod_entry_skip = false;
    for (const FunctionEntryPlugin& plugin : function_entry_plugins()) {
        if (plugin.address == address) plugin.callback(cpu, address);
    }
    const int skip = s_mod_entry_skip ? 1 : 0;
    s_mod_entry_cpu = outer_cpu;
    s_mod_entry_skip = outer_skip;
    return skip;
}
