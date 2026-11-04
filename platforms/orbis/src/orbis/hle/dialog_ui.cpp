// The system dialogs' drawing helpers (orbis/hle/dialog_ui.h).

#include "orbis/hle/dialog_ui.h"

#include "runtime/ui/canvas.h"

namespace recomp::orbis::dialog {

std::vector<std::string> Wrap(const std::string& s, int size, int w) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t nl = s.find('\n', start);
    if (nl == std::string::npos) nl = s.size();
    const std::string para = s.substr(start, nl - start);
    std::string line;
    for (size_t i = 0; i <= para.size();) {
      size_t sp = para.find(' ', i);
      if (sp == std::string::npos) sp = para.size();
      const std::string word = para.substr(i, sp - i);
      const std::string next = line.empty() ? word : line + " " + word;
      if (!line.empty() && recomp::ui::TextWidth(size, next) > w) {
        out.push_back(line);
        line = word;
      } else {
        line = next;
      }
      i = sp + 1;
    }
    out.push_back(line);
    start = nl + 1;
  }
  return out;
}

}  // namespace recomp::orbis::dialog
