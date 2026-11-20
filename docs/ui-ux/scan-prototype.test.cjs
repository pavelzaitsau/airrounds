const assert = require("node:assert/strict");
const { test } = require("node:test");
const { createScanPrototype } = require("./scan-prototype.js");

function fakeClock() {
    let now = 0;
    let sequence = 0;
    const timers = new Map();

    function next(limit = Infinity) {
        let found;
        for (const [id, timer] of timers) {
            if (timer.at <= limit && (!found || timer.at < found[1].at)) found = [id, timer];
        }
        return found;
    }

    function run([id, timer]) {
        timers.delete(id);
        now = timer.at;
        timer.action();
    }

    return {
        setTimeout(action, delay) {
            sequence++;
            timers.set(sequence, { at: now + delay, action });
            return sequence;
        },
        clearTimeout(id) { timers.delete(id); },
        step() {
            const found = next();
            if (found) run(found);
            return Boolean(found);
        },
        advance(ms) {
            const end = now + ms;
            for (let found = next(end); found; found = next(end)) run(found);
            now = end;
        },
        pending: () => timers.size,
    };
}

function fixture(options = {}) {
    const clock = fakeClock();
    let changes = 0;
    const app = createScanPrototype({ clock, onChange: () => changes++, ...options });
    return {
        app,
        clock,
        changes: () => changes,
        press: (...keys) => keys.forEach((key) => app.press(key)),
        screen: () => app.screen(),
        state: () => app.state(),
    };
}

function openCaptureMode(f) {
    f.press("down", "ok");
    assert.equal(f.screen().view, "mode");
}

function currentMode(f) {
    return f.screen().rows.find((row) => row.current).id;
}

function focusedMode(f) {
    return f.screen().rows.find((row) => row.focused).id;
}

// Opens the scan list from the main menu and waits for the scan to finish.
function openList(f) {
    f.press("ok");
    assert.equal(f.screen().view, "scanning");
    f.clock.advance(950);
    assert.equal(f.screen().view, "list");
}

// Captures the first network in a one-point round and returns to the list.
function captureFirst(f) {
    f.press("ok", "right");
    assert.equal(f.screen().view, "running");
    f.clock.advance(2300 + 500);
    assert.equal(f.screen().round.phase, "done");
    f.clock.advance(1550);
    assert.equal(f.screen().view, "list");
    assert.deepEqual(f.state().saved, [0]);
}

function selectTodoA(f) {
    openCaptureMode(f);
    f.press("down", "ok");
    assert.equal(f.screen().view, "home");
    assert.equal(f.state().mode, "todoA");
}

test("main menu lists five items in order and scrolls to About", () => {
    const f = fixture();
    const page = f.screen();
    assert.deepEqual(page.items, ["Scan area", "Capture mode", "Flash board hardware", "Test board", "About"]);
    assert.equal(page.rows.length, 4);
    assert.equal(page.above, false);
    assert.equal(page.below, true);
    f.press("down", "down", "down", "down");
    const end = f.screen();
    assert.equal(end.items[end.focus], "About");
    assert.deepEqual(end.rows.map((row) => row.label), ["Capture mode", "Flash board hardware", "Test board", "About"]);
    assert.equal(end.above, true);
    assert.equal(end.below, false);
    f.press("down");
    assert.equal(f.screen().focus, 0, "Down on About wraps to the first item");
    assert.equal(f.screen().offset, 0);
    f.press("up");
    assert.equal(f.screen().items[f.screen().focus], "About", "Up on the first item wraps to About");
    f.press("up");
    assert.equal(f.screen().focus, 3);
    assert.equal(f.screen().offset, 0, "the menu scrolls back as the app does");
    f.press("up", "up", "up");
    assert.equal(f.screen().focus, 0);
    assert.equal(f.screen().offset, 0);
});

test("OK on Capture mode opens the screen with the marker and focus on Beacon", () => {
    const f = fixture();
    openCaptureMode(f);
    const page = f.screen();
    assert.equal(page.title, "Capture mode");
    assert.deepEqual(page.rows.map((row) => row.label), ["Beacon", "Todo A", "Todo B"]);
    assert.equal(currentMode(f), "beacon");
    assert.equal(focusedMode(f), "beacon");
    assert.deepEqual(page.rows.map((row) => row.right), [undefined, "Soon", "Soon"]);
    assert.equal(page.hint, null);
});

test("OK on Beacon returns to the menu with focus on Capture mode", () => {
    const f = fixture();
    openCaptureMode(f);
    f.press("ok");
    const page = f.screen();
    assert.equal(page.view, "home");
    assert.equal(page.items[page.focus], "Capture mode");
    assert.equal(f.state().mode, "beacon");
});

test("OK on Todo A keeps Beacon and shows Not available yet until the next key", () => {
    const f = fixture();
    openCaptureMode(f);
    f.press("down", "ok");
    assert.equal(f.screen().view, "mode");
    assert.equal(f.screen().hint, "Not available yet");
    assert.equal(currentMode(f), "beacon");
    f.press("up");
    assert.equal(f.screen().hint, null);
    assert.equal(focusedMode(f), "beacon");
});

test("Back returns to the menu and changes nothing", () => {
    const f = fixture({ implemented: ["beacon", "todoA"] });
    openCaptureMode(f);
    f.press("down", "back");
    const page = f.screen();
    assert.equal(page.view, "home");
    assert.equal(page.items[page.focus], "Capture mode");
    assert.equal(f.state().mode, "beacon");
});

test("Session active locks the choice during a round", () => {
    const f = fixture({ implemented: ["beacon", "todoA"], board: { session: "running" } });
    assert.equal(f.screen().items[0], "Session status");
    f.press("ok");
    assert.equal(f.screen().view, "home", "Session status has no round to show in the mockup");
    openCaptureMode(f);
    assert.equal(f.screen().hint, "Session active");
    f.press("down", "ok");
    assert.equal(f.screen().view, "mode");
    assert.equal(f.state().mode, "beacon");
    assert.equal(f.screen().hint, "Session active");
});

test("Scan active locks the choice during a scan or a pending scan request", () => {
    for (const scan of ["running", "pending"]) {
        const f = fixture({ implemented: ["beacon", "todoA"], board: { scan } });
        openCaptureMode(f);
        assert.equal(f.screen().hint, "Scan active");
        f.press("down", "ok");
        assert.equal(f.state().mode, "beacon");
    }
});

test("Board state unknown wins over the last known session state", () => {
    for (const link of ["lost", "offline", "checking", "update"]) {
        const f = fixture({ implemented: ["beacon", "todoA"], board: { link, session: "running" } });
        openCaptureMode(f);
        assert.equal(f.screen().hint, "Board state unknown", link);
        f.press("down", "ok");
        assert.equal(f.state().mode, "beacon");
    }
    const pending = fixture({ board: { pending: true } });
    openCaptureMode(pending);
    assert.equal(pending.screen().hint, "Board state unknown");
});

test("a restarted board is confirmed and idle", () => {
    const f = fixture({ implemented: ["beacon", "todoA"], board: { link: "restarted", modes: ["beacon", "todoA"] } });
    openCaptureMode(f);
    assert.equal(f.screen().hint, null);
    f.press("down", "ok");
    assert.equal(f.state().mode, "todoA");
});

test("a lock hint stays while Up and Down move the focus", () => {
    const f = fixture({ board: { session: "finalizing" } });
    openCaptureMode(f);
    f.press("down");
    assert.equal(f.screen().hint, "Session active");
    assert.equal(focusedMode(f), "todoA");
    f.press("down");
    assert.equal(focusedMode(f), "todoB");
    f.press("up");
    assert.equal(f.screen().hint, "Session active");
    assert.equal(focusedMode(f), "todoA");
});

test("the lock hint wins over Not available yet", () => {
    const f = fixture({ board: { scan: "running" } });
    openCaptureMode(f);
    f.press("down", "ok");
    assert.equal(f.screen().hint, "Scan active");
    f.app.setBoard({ scan: "idle" });
    assert.equal(f.screen().hint, null);
    f.press("ok");
    assert.equal(f.screen().hint, "Not available yet");
});

test("an implemented and listed mode can be selected", () => {
    const f = fixture({ implemented: ["beacon", "todoA"] });
    openCaptureMode(f);
    assert.deepEqual(f.screen().rows.map((row) => row.available), [true, true, false]);
    f.press("down", "ok");
    const page = f.screen();
    assert.equal(page.view, "home");
    assert.equal(page.items[page.focus], "Capture mode");
    assert.equal(f.state().mode, "todoA");
});

test("no other mode is available until the modes reply arrives", () => {
    const f = fixture({ implemented: ["beacon", "todoA"], board: { modes: undefined } });
    assert.deepEqual(f.state().available, ["beacon"]);
    f.app.modesReply(null);
    assert.deepEqual(f.state().available, ["beacon"]);
    f.app.modesReply(["beacon", "todoA"]);
    assert.deepEqual(f.state().available, ["beacon", "todoA"]);
});

test("the marker stays on the current mode wherever the focus is", () => {
    const f = fixture({ implemented: ["beacon", "todoA"] });
    selectTodoA(f);
    f.press("ok");
    assert.equal(focusedMode(f), "todoA");
    for (const key of ["up", "down", "down"]) {
        f.press(key);
        assert.equal(currentMode(f), "todoA");
        assert.equal(f.screen().rows.filter((row) => row.current).length, 1);
    }
    assert.equal(focusedMode(f), "todoB");
});

test("a modes reply without the selected mode returns to Beacon and clears results", () => {
    const f = fixture({ implemented: ["beacon", "todoA"] });
    selectTodoA(f);
    f.press("up");
    openList(f);
    captureFirst(f);
    f.app.modesReply(["beacon", "todoA"]);
    assert.equal(f.state().mode, "todoA");
    assert.deepEqual(f.state().saved, [0]);
    f.app.modesReply(["beacon"]);
    assert.equal(f.state().mode, "beacon");
    assert.deepEqual(f.state().saved, []);
    assert.equal(f.screen().rows[0].saved, false);
});

test("losing the capture-mode capability returns to Beacon and clears results", () => {
    const f = fixture({ implemented: ["beacon", "todoA"] });
    selectTodoA(f);
    f.press("up");
    openList(f);
    captureFirst(f);
    f.app.setBoard({ captureModes: false });
    assert.equal(f.state().mode, "beacon");
    assert.deepEqual(f.state().saved, []);
    assert.deepEqual(f.state().available, ["beacon"]);
    f.app.modesReply(["beacon", "todoA"]);
    assert.equal(f.state().board.modes, undefined);
});

test("a reconnect without a modes reply keeps the choice and shows Mode pending", () => {
    const f = fixture({ implemented: ["beacon", "todoA"] });
    selectTodoA(f);
    f.press("up");
    openList(f);
    f.press("ok");
    assert.equal(f.screen().start, true);
    assert.equal(f.screen().heading, "1/5 selected");
    f.app.setBoard({ link: "lost" });
    f.app.setBoard({ link: "ready" });
    assert.equal(f.state().mode, "todoA");
    let page = f.screen();
    assert.equal(page.heading, "Mode pending");
    assert.equal(page.start, false);
    assert.equal(page.rows[0].selected, true);
    f.press("right");
    assert.equal(f.screen().view, "list");
    f.app.modesReply(["beacon", "todoA"]);
    page = f.screen();
    assert.equal(page.heading, "1/5 selected");
    assert.equal(page.start, true);
    assert.equal(f.state().mode, "todoA");
});

test("the list heading shows Maximum 5, then Board offline, then Mode pending", () => {
    const f = fixture({ implemented: ["beacon", "todoA"] });
    selectTodoA(f);
    f.press("up");
    openList(f);
    f.app.setBoard({ link: "lost" });
    for (let index = 0; index < 6; index++) {
        f.press("ok");
        if (index < 5) f.press("down");
    }
    assert.equal(f.screen().heading, "Maximum 5");
    f.press("up");
    assert.equal(f.screen().heading, "Board offline");
    assert.equal(f.screen().start, false, "Start needs a confirmed board");
    f.press("right");
    assert.equal(f.screen().view, "list", "Right does not start a round the screen does not offer");
    f.app.setBoard({ link: "ready" });
    assert.equal(f.screen().heading, "Mode pending");
    assert.equal(f.screen().start, false);
    f.app.modesReply(["beacon", "todoA"]);
    assert.equal(f.screen().heading, "5/5 selected");
    assert.equal(f.screen().start, true);
});

test("a mode change clears the results but keeps the list and the selection", () => {
    const f = fixture({ implemented: ["beacon", "todoA"] });
    openList(f);
    captureFirst(f);
    f.press("down", "ok");
    assert.deepEqual(f.state().selected, [1]);
    const networks = f.state().networks;
    f.press("back");
    selectTodoA(f);
    assert.deepEqual(f.state().saved, []);
    assert.deepEqual(f.state().missed, []);
    assert.deepEqual(f.state().selected, [1]);
    assert.deepEqual(f.state().networks, networks);
    f.press("up", "ok");
    f.clock.advance(950);
    const page = f.screen();
    assert.equal(page.rows[0].saved, false);
    assert.equal(page.rows[1].selected, true);
});

test("selecting the current mode again keeps the results", () => {
    const f = fixture();
    openList(f);
    captureFirst(f);
    f.press("back", "down", "ok", "ok");
    assert.deepEqual(f.state().saved, [0]);
});

test("a new instance starts on Beacon", () => {
    const first = fixture({ implemented: ["beacon", "todoA"] });
    selectTodoA(first);
    const reloaded = fixture({ implemented: ["beacon", "todoA"] });
    assert.equal(reloaded.state().mode, "beacon");
    openCaptureMode(reloaded);
    assert.equal(currentMode(reloaded), "beacon");
});

test("default page keeps Back inert during a round and runs timers through the clock", () => {
    const f = fixture();
    openList(f);
    f.press("ok", "right");
    f.press("back");
    assert.equal(f.screen().view, "running");
    assert.equal(f.screen().simulation.visible, true);
    const before = f.changes();
    assert.equal(f.clock.step(), true);
    assert.equal(f.screen().round.phase, "save");
    assert.ok(f.changes() > before);
    f.clock.step();
    assert.equal(f.screen().round.phase, "done");
    f.press("ok");
    assert.equal(f.screen().view, "list");
    assert.equal(f.clock.pending(), 0);
});

test("Right does not start a second round while the board runs one", () => {
    const f = fixture();
    openList(f);
    f.press("ok");
    f.app.setBoard({ session: "running" });
    assert.equal(f.screen().start, false);
    f.press("right");
    assert.equal(f.screen().view, "list");
});

test("Right does not start a round while a scan runs or waits on the board", () => {
    for (const scan of ["running", "pending"]) {
        const f = fixture();
        openList(f);
        f.press("ok");
        f.app.setBoard({ scan });
        assert.equal(f.screen().start, false);
        f.press("right");
        assert.equal(f.screen().view, "list");
    }
});

test("the network list wraps its focus and scrolls as the app does", () => {
    const f = fixture();
    openList(f);
    f.press("up");
    assert.equal(f.screen().rows.findIndex((row) => row.focused), 5);
    assert.equal(f.screen().offset, 3);
    f.press("down");
    assert.equal(f.screen().rows.findIndex((row) => row.focused), 0);
    assert.equal(f.screen().offset, 0);
});

test("a click on a visible row keeps the list where it is", () => {
    const f = fixture();
    openList(f);
    f.press("up");
    assert.equal(f.screen().offset, 3);
    f.app.select(3);
    assert.equal(f.screen().offset, 3);
    assert.equal(f.state().selected.includes(3), true);
});
