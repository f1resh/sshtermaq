#include "app/PageHandover.h"

#include "bridge/Bridge.h"
#include "core/Log.h"

namespace st {

PageHandover::PageHandover(Bridge& bridge, std::function<int()> closeOldPage)
    : bridge_(bridge), closeOldPage_(std::move(closeOldPage)) {}

void PageHandover::registerHandlers() {
    auto state = state_;
    bridge_.on("app:setReloadSnapshot", [state](const Json& a) { state->workspace = jsonArg(a, 0); });
    bridge_.handleSync("app:takeReloadInfo", [this](const Json&) { return takeReloadInfo(); });
}

void PageHandover::pageLoading() {
    bridge_.newPage();
    if (state_->loads++ == 0) return;  // the first page: nothing before it
    const int closed = closeOldPage_ ? closeOldPage_() : 0;
    state_->reloaded = true;
    state_->closed = closed;
}

Json PageHandover::takeReloadInfo() {
    Json info = {{"reloaded", state_->reloaded}, {"closedConnections", state_->closed}};
    if (state_->reloaded && !state_->workspace.is_null()) info["workspace"] = state_->workspace;
    state_->reloaded = false;
    state_->closed = 0;
    return info;
}

}  // namespace st
