// PageHandover (app/PageHandover.h): what a reloaded page gets from the host. The first page closes nothing; a reload
// closes the old page's connections once and tells the new page so, with the old page's last workspace.
#include "app/PageHandover.h"

#include "TestBridge.h"

#include <catch2/catch_test_macros.hpp>

using namespace st;
using namespace st::test;

TEST_CASE("page handover: the first page closes nothing and is not a reload", "[app][reload]") {
    BridgeHarness h;
    int closes = 0;
    PageHandover handover(h.bridge, [&] {
        ++closes;
        return 3;
    });
    handover.registerHandlers();
    handover.pageLoading();
    h.send("app:setReloadSnapshot", Json::array({Json{{"tabs", Json::array()}}}));
    CHECK(closes == 0);
    const Json info = h.invoke("app:takeReloadInfo", Json::array());
    CHECK(info["reloaded"] == false);
    CHECK(info["closedConnections"] == 0);
    CHECK_FALSE(info.contains("workspace"));
}

TEST_CASE("page handover: a reload closes the old page's connections and hands over its workspace once", "[app][reload]") {
    BridgeHarness h;
    int closes = 0;
    PageHandover handover(h.bridge, [&] {
        ++closes;
        return 2;
    });
    handover.registerHandlers();
    handover.pageLoading();
    const Json snapshot = {{"version", 1}, {"tabs", Json::array({Json{{"title", "prod"}}})}};
    h.send("app:setReloadSnapshot", Json::array({snapshot}));

    handover.pageLoading();  // the renderer process died: the page loads again
    CHECK(closes == 1);
    Json info = h.invoke("app:takeReloadInfo", Json::array());
    CHECK(info["reloaded"] == true);
    CHECK(info["closedConnections"] == 2);
    CHECK(info["workspace"] == snapshot);

    // Asked again (the same page): no longer a reload.
    info = h.invoke("app:takeReloadInfo", Json::array());
    CHECK(info["reloaded"] == false);
    CHECK_FALSE(info.contains("workspace"));

    // Another reload: closed again; the latest snapshot is handed over.
    const Json later = {{"version", 1}, {"tabs", Json::array()}};
    h.send("app:setReloadSnapshot", Json::array({later}));
    handover.pageLoading();
    CHECK(closes == 2);
    info = h.invoke("app:takeReloadInfo", Json::array());
    CHECK(info["reloaded"] == true);
    CHECK(info["workspace"] == later);
}

TEST_CASE("page handover: a reload before the page sent a workspace hands over none", "[app][reload]") {
    BridgeHarness h;
    PageHandover handover(h.bridge, [] { return 0; });
    handover.registerHandlers();
    handover.pageLoading();
    handover.pageLoading();
    const Json info = h.invoke("app:takeReloadInfo", Json::array());
    CHECK(info["reloaded"] == true);
    CHECK(info["closedConnections"] == 0);
    CHECK_FALSE(info.contains("workspace"));
}
