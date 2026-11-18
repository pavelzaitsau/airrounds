const assert = require("node:assert/strict");
const { test } = require("node:test");
const { createPrototype } = require("./prototype.js");

function fixture(profile) {
    let saved;
    const app = createPrototype({
        profile,
        saveProfile(points) { saved = points; },
    });

    function press(...keys) {
        for (const key of keys) {
            app.press(key);
            app.release(key);
        }
    }

    return {
        app,
        press,
        step: () => app.step(),
        link: () => app.toggleLink(),
        screen: () => app.screen(),
        title: () => app.screen().title,
        saved: () => saved,
    };
}

function start(f) {
    f.press("ok");
    assert.equal(f.title(), "Review session");
    f.press("ok");
    assert.equal(f.title(), "Starting...");
    f.step();
    assert.equal(f.title(), "Observing");
}

test("starts the shown profile with two OK presses", () => {
    const f = fixture();
    start(f);
    assert.equal(f.screen().action, "Stop");
    assert.deepEqual(f.screen().lines, ["Point 1 / Ch 6"]);
});

test("waits for Stop confirmation and file finalization", () => {
    const f = fixture();
    start(f);
    f.press("ok", "ok");
    assert.equal(f.title(), "Stopping...");
    f.step();
    assert.equal(f.title(), "Reception stopped");
    f.press("ok");
    assert.equal(f.title(), "Reception stopped");
    f.step();
    assert.equal(f.title(), "Session stopped");
    f.press("ok");
    assert.deepEqual(f.screen().rows.map(({ detail }) => detail), ["Stopped", "Stopped", "Stopped"]);
});

test("recovers a completed session after losing the Stop reply", () => {
    const f = fixture();
    start(f);
    f.press("ok");
    f.link();
    assert.equal(f.title(), "Link lost");
    f.step();
    f.step();
    f.press("ok");
    assert.equal(f.title(), "Link lost");
    f.link();
    assert.equal(f.title(), "Checking status");
    f.step();
    assert.equal(f.title(), "Session stopped");
});

test("does not execute an action that changed while OK was held", () => {
    const f = fixture();
    f.app.press("ok");
    f.link();
    f.app.release("ok");
    assert.equal(f.title(), "AirRounds");
    assert.equal(f.screen().rows[0].label, "Board status");

    f.link();
    f.step();
    start(f);
    f.app.press("ok");
    for (let i = 0; i < 6; i++) f.step();
    f.app.release("ok");
    assert.equal(f.title(), "Session ended");
    f.press("ok");
    assert.equal(f.title(), "Results");
});

test("Back leaves the status and reopening the app finds the running visit", () => {
    const f = fixture();
    start(f);
    f.press("back");
    assert.equal(f.screen().rows[0].label, "Session status");
    f.press("back");
    assert.equal(f.title(), "Apps");
    f.press("ok");
    assert.equal(f.title(), "Observing");
});

test("opening the app offline resumes only after the board confirms the session", () => {
    const f = fixture();
    start(f);
    f.press("back", "back");
    f.link();
    f.press("ok");
    assert.equal(f.title(), "AirRounds");
    f.link();
    assert.equal(f.title(), "AirRounds");
    f.step();
    assert.equal(f.title(), "Observing");
});

test("shows mixed results and gives directions to a saved file", () => {
    const f = fixture();
    start(f);
    for (let i = 0; i < 6; i++) f.step();
    assert.deepEqual(f.screen().lines, ["1 of 3 saved"]);
    f.press("ok");
    assert.deepEqual(f.screen().rows.map(({ detail }) => detail), ["Saved", "No sample", "Not seen"]);
    f.press("ok", "right");
    assert.deepEqual(f.screen().lines, ["session-00000001/", "point-01.pcap"]);
    f.press("back", "back");
    assert.equal(f.title(), "Results");
});

test("an empty offline profile accepts BSSID and channel through the device keys", () => {
    const f = fixture([]);
    f.link();
    f.press("ok", "ok");
    assert.equal(f.title(), "Enter BSSID");
    f.press("ok");
    f.press("right", "right", "ok");
    f.press("left", "left");
    for (let i = 0; i < 9; i++) f.press("ok");
    f.press("right", "ok");
    f.press("down");
    for (let i = 0; i < 7; i++) f.press("right");
    f.press("ok");
    assert.equal(f.title(), "Select channel");
    f.press("right", "ok");
    assert.deepEqual(f.saved(), [{ address: "020000000001", channel: 2 }]);
    f.press("back");
    assert.equal(f.screen().rows[0].label, "Board status");
});

test("can stop during point saving without starting another point", () => {
    const f = fixture();
    start(f);
    f.step();
    assert.equal(f.title(), "Saving point");
    f.press("ok");
    f.step();
    f.step();
    assert.equal(f.title(), "Session stopped");
    f.press("ok");
    assert.deepEqual(f.screen().rows.map(({ detail }) => detail), ["Saved", "Stopped", "Stopped"]);
});

test("Back discards an address edit and returns to the point", () => {
    const f = fixture();
    f.press("down", "ok", "ok", "ok");
    assert.equal(f.title(), "Enter BSSID");
    f.press("right", "ok", "back");
    assert.equal(f.title(), "Point 1");
    assert.equal(f.screen().subheading, "02:1A:2B:3C:4D:51");
    assert.equal(f.saved(), undefined);
});

test("results keep the point list even after editing the saved profile", () => {
    const f = fixture();
    start(f);
    f.press("back", "down", "ok", "ok", "down", "ok", "ok");
    assert.equal(f.saved().length, 2);
    f.press("back", "ok");
    for (let i = 0; i < 6; i++) f.step();
    assert.deepEqual(f.screen().lines, ["1 of 3 saved"]);
    f.press("ok", "ok");
    assert.equal(f.screen().lines[1], "02:1A:2B:3C:4D:51");
});
