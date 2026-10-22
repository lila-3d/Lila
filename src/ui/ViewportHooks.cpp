#include "ui/ViewportHooks.h"

namespace lila::ui::viewportHooks
{
namespace
{
std::vector<Drawer>& drawerList()
{
    static std::vector<Drawer> v;
    return v;
}

std::vector<Picker>& pickerList()
{
    static std::vector<Picker> v;
    return v;
}
} // namespace

void addDrawer(Drawer fn) { drawerList().push_back(std::move(fn)); }
void addPicker(Picker fn) { pickerList().push_back(std::move(fn)); }
const std::vector<Drawer>& drawers() { return drawerList(); }
const std::vector<Picker>& pickers() { return pickerList(); }
} // namespace lila::ui::viewportHooks
