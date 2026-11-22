#pragma once
// A reloaded page (its renderer process died, or a reload from the dev tools) knows nothing of the old page's tabs:
// the old page's connections and file sessions are closed, quietly, and the new page opens its own. So that it can
// open the same tabs, the page keeps its workspace in the host's memory (app:setReloadSnapshot, never on disk, also
// when "Restore tabs on startup" is off), and the new page asks once whether it replaced one (app:takeReloadInfo).
// UI thread only (bridge handlers and HostController::onPageLoading).
#include "core/Json.h"

#include <functional>
#include <memory>

namespace st {

class Bridge;

class PageHandover {
public:
    /** `closeOldPage`: closes what the previous page had open and returns how many of its connections were open. */
    PageHandover(Bridge& bridge, std::function<int()> closeOldPage);

    /** Registers app:setReloadSnapshot and app:takeReloadInfo. */
    void registerHandlers();

    /** A page starts loading (HostController::onPageLoading): the bridge drops what was meant for the previous page,
     *  and from the second load on the previous page's connections are closed. */
    void pageLoading();

    /** {reloaded, closedConnections, workspace?}; reloaded only once per reload. */
    Json takeReloadInfo();

    int loads() const { return state_->loads; }

private:
    struct State {
        int loads = 0;
        bool reloaded = false;
        int closed = 0;
        Json workspace;
    };
    Bridge& bridge_;
    std::function<int()> closeOldPage_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace st
