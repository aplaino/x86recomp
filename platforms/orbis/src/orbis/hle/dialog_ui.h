#pragma once
// What the system dialogs the runtime draws (libSceSaveDataDialog,
// libSceMsgDialog) share: the dialog the player sees is this runtime's
// own design, not the console's.

#include <string>
#include <vector>

namespace recomp::orbis::dialog {

// Lines of text no wider than w pixels at text size `size`, broken at the
// newlines and spaces.
std::vector<std::string> Wrap(const std::string& s, int size, int w);

}  // namespace recomp::orbis::dialog
