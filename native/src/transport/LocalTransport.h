#pragma once
// Local terminal: a shell (or any program) on this computer in a pseudo-terminal (platform/sys/Pty.h: ConPTY on
// Windows, a pty on macOS). The session's LocalOptions give the command and start folder, ConnectionOptions its
// environment and TERM. Output is UTF-8. Local echo/edit default off (the shell does both). The close reports the
// program's exit code; a clean close is exit code 0 (or the user closing the tab).
#include "transport/Transport.h"

#include <memory>
#include <vector>

namespace st {

std::unique_ptr<Transport> makeLocalTransport(TransportContext ctx);

/** The shells installed here (sys::pty::listShells), for the "New local terminal" menu. */
std::vector<LocalShellInfo> listLocalShells();

}  // namespace st
