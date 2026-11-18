"use strict";

const demoProfile = [
    { address: "021A2B3C4D51", channel: 6 },
    { address: "021A2B3C4D52", channel: 6 },
    { address: "021A2B3C4D53", channel: 11 },
];

const outcomes = {
    saved: ["Saved", "Saved to board SD"],
    noSample: ["No sample", "No usable sample"],
    notSeen: ["Not seen", "Point not seen"],
    stopped: ["Stopped", "Stopped before result"],
};
const demoOutcomes = ["saved", "noSample", "notSeen"];

const hexKeys = ["01234567", "89ABCDEF"];
const busyPhases = new Set(["starting", "observing", "writing", "stopping", "finalizing"]);
const isBusy = (session) => Boolean(session && busyPhases.has(session.phase));
const formatAddress = (address) => address.match(/../g).join(":");
const pointName = (index) => `Point ${index + 1}`;
const wrap = (value, length) => (value + length) % length;

function createPrototype({ profile = demoProfile, saveProfile = () => {} } = {}) {
    let points = structuredClone(profile);
    let view = "home";
    let focus = 0;
    let pointIndex = 0;
    let draft;
    let removeFailed = false;
    let link = "ready";
    let boardSession = null;
    let session = null;
    let lastResult = null;
    let sequence = 0;
    let heldAction;
    let result;
    let resultOrigin;
    let reopenPending = false;

    function go(next, selected = 0) {
        view = next;
        focus = selected;
    }

    function homeRows() {
        const primary = isBusy(session)
            ? ["Session status", "session"]
            : points.length === 0
                ? ["Set up points", "points"]
                : link === "ready" ? ["Start session", "review"] : ["Board status", "connection"];
        return [primary, ["Access points", "points"], ["Last result", "last"]];
    }

    function menu(title, entries) {
        return {
            title,
            rows: entries.map((entry, i) => ({
                label: Array.isArray(entry) ? entry[0] : entry,
                detail: Array.isArray(entry) ? entry[1] : undefined,
                selected: i === focus,
            })),
        };
    }

    function boardStatus() {
        if (link === "checking") return { title: "Checking status", lines: ["Waiting for board"] };
        return { title: "Devboard offline", lines: ["Check cable"] };
    }

    function outcomeScreen(report) {
        return {
            title: report.phase === "stopped" ? "Session stopped" : "Session ended",
            lines: [`${report.outcomes.filter((item) => item === "saved").length} of ${report.points.length} saved`],
            action: "Results",
        };
    }

    function sessionScreen() {
        if (link === "offline" && isBusy(session)) {
            return { title: "Link lost", lines: ["Session may run", "Check cable"] };
        }
        if (link !== "ready") return boardStatus();
        if (!session) return { title: "No session" };
        const channel = session.points[session.index].channel;
        switch (session.phase) {
            case "starting": return { title: "Starting...", lines: ["Waiting for board"] };
            case "observing": return {
                title: "Observing", lines: [`${pointName(session.index)} / Ch ${channel}`], action: "Stop",
            };
            case "writing": return {
                title: "Saving point", lines: [pointName(session.index)], action: "Stop",
            };
            case "stopping": return { title: "Stopping...", lines: ["Waiting for board"] };
            case "finalizing": return { title: "Reception stopped", lines: ["Saving to board SD"] };
            default: return outcomeScreen(session);
        }
    }

    function screen() {
        switch (view) {
            case "home": return menu("AirRounds", homeRows().map(([label]) => label));
            case "connection": return boardStatus();
            case "closed": return menu("Apps", ["AirRounds"]);
            case "points": return menu("Access points", [
                ...points.map((point, i) => [pointName(i), `Ch ${point.channel}`]),
                ...(points.length < 3 ? ["Add point"] : []),
            ]);
            case "point": {
                const page = menu(pointName(pointIndex), ["Edit", "Remove"]);
                return { ...page, subheading: formatAddress(points[pointIndex].address) };
            }
            case "remove": return {
                title: removeFailed ? "Profile not saved" : `Remove ${pointName(pointIndex)}?`,
                lines: [formatAddress(points[pointIndex].address)],
                action: "Remove",
            };
            case "address": return {
                title: draft.error || "Enter BSSID",
                hex: { address: draft.address, cursor: draft.cursor, row: draft.row, col: draft.col },
            };
            case "channel": return {
                title: draft.error || "Select channel",
                lines: [formatAddress(draft.address), `< Channel ${draft.channel} >`],
                action: "Save",
            };
            case "review": {
                const ready = link === "ready" && !isBusy(session);
                return {
                    title: "Review session",
                    profile: points,
                    action: ready ? "Start" : undefined,
                    hint: ready ? undefined : isBusy(session) ? "Session in progress" : boardStatus().title,
                };
            }
            case "session": return sessionScreen();
            case "last": return lastResult ? outcomeScreen(lastResult) : { title: "No result available" };
            case "results": return menu("Results", result.outcomes.map((item, i) => [
                pointName(i), outcomes[item][0],
            ]));
            case "detail": {
                const point = result.points[focus];
                const item = result.outcomes[focus];
                return {
                    title: pointName(focus),
                    lines: [
                        outcomes[item][1], formatAddress(point.address),
                        `Ch ${point.channel}${item === "saved" ? "     File >" : ""}`,
                    ],
                };
            }
            case "file": return {
                title: "Board SD file",
                lines: [
                    `session-${result.id.toString(16).padStart(8, "0").toUpperCase()}/`,
                    `point-${String(focus + 1).padStart(2, "0")}.pcap`,
                ],
            };
        }
    }

    function publishSnapshot() {
        session = boardSession ? structuredClone(boardSession) : null;
        if (session && !isBusy(session)) lastResult = session;
    }

    function nextEvent() {
        if (link === "checking") return "Read current session";
        switch (boardSession?.phase) {
            case "starting": return "Acknowledge Start";
            case "observing": return "Finish point observation";
            case "writing": return "Publish point result";
            case "stopping": return "Acknowledge Stop";
            case "finalizing": return "Finish saving";
            default: return "";
        }
    }

    function step() {
        if (link === "checking") {
            link = "ready";
            publishSnapshot();
            if (reopenPending && view === "home" && isBusy(session)) go("session");
            reopenPending = false;
            return;
        }
        if (!boardSession) return;

        switch (boardSession.phase) {
            case "starting":
                boardSession.phase = "observing";
                break;
            case "observing":
                boardSession.phase = "writing";
                break;
            case "writing": {
                boardSession.outcomes.push(demoOutcomes[boardSession.index]);
                if (boardSession.index + 1 < boardSession.points.length) {
                    boardSession.index++;
                    boardSession.phase = "observing";
                } else {
                    boardSession.phase = "ended";
                }
                break;
            }
            case "stopping":
                boardSession.phase = "finalizing";
                break;
            case "finalizing":
                if (boardSession.writeOnStop) {
                    boardSession.outcomes.push(demoOutcomes[boardSession.index]);
                }
                while (boardSession.outcomes.length < boardSession.points.length) {
                    boardSession.outcomes.push("stopped");
                }
                boardSession.phase = "stopped";
                break;
        }
        if (link === "ready") publishSnapshot();
    }

    function toggleLink() {
        heldAction = undefined;
        link = link === "offline" ? "checking" : "offline";
    }

    function reset(loadDemo) {
        if (!commitProfile(loadDemo ? demoProfile : [])) return false;
        link = "ready";
        session = boardSession = lastResult = result = null;
        heldAction = draft = undefined;
        sequence = 0;
        reopenPending = false;
        go("home");
        return true;
    }

    function commitProfile(next) {
        try {
            saveProfile(structuredClone(next));
        } catch {
            return false;
        }
        points = structuredClone(next);
        return true;
    }

    function edit(index) {
        const point = points[index];
        draft = {
            index, address: point?.address ?? "000000000000", channel: point?.channel ?? 1,
            cursor: 0, row: 0, col: 0, error: "", previousView: view,
        };
        go("address");
    }

    function updateNibble(character) {
        draft.address = draft.address.slice(0, draft.cursor) + character +
            draft.address.slice(draft.cursor + 1);
        draft.error = "";
    }

    function enterAddress() {
        if (draft.col < 8 && draft.row >= 0) {
            updateNibble(hexKeys[draft.row][draft.col]);
            draft.cursor = Math.min(draft.cursor + 1, 11);
        } else if (draft.row === 0) {
            draft.cursor = Math.max(0, draft.cursor - 1);
            updateNibble("0");
        } else if (draft.row === 1) {
            if (/^0{12}$/.test(draft.address) || (parseInt(draft.address[1], 16) & 1)) {
                draft.error = "Invalid BSSID";
            } else if (points.some((point, i) => i !== draft.index && point.address === draft.address)) {
                draft.error = "Point already added";
            } else {
                draft.error = "";
                go("channel");
            }
        }
    }

    function openResults(report, origin) {
        result = report;
        resultOrigin = origin;
        go("results");
    }

    function ok() {
        switch (view) {
            case "home": go(homeRows()[focus][1]); break;
            case "closed":
                reopenPending = link !== "ready";
                go(link === "ready" && isBusy(session) ? "session" : "home");
                break;
            case "points":
                if (focus === points.length) edit(focus);
                else {
                    pointIndex = focus;
                    go("point");
                }
                break;
            case "point":
                if (focus === 0) edit(pointIndex);
                else {
                    removeFailed = false;
                    go("remove");
                }
                break;
            case "remove": {
                if (commitProfile(points.filter((_, i) => i !== pointIndex))) {
                    go("points", Math.max(0, Math.min(pointIndex, points.length - 1)));
                } else {
                    removeFailed = true;
                }
                break;
            }
            case "address": enterAddress(); break;
            case "channel": {
                const next = [...points];
                next[draft.index] = { address: draft.address, channel: draft.channel };
                if (commitProfile(next)) go("points", draft.index);
                else draft.error = "Profile not saved";
                break;
            }
            case "review":
                if (link === "ready" && !isBusy(session) && points.length > 0) {
                    boardSession = {
                        id: ++sequence, phase: "starting", points: structuredClone(points),
                        index: 0, outcomes: [], writeOnStop: false,
                    };
                    publishSnapshot();
                    go("session");
                }
                break;
            case "session":
                if (link !== "ready" || !session) break;
                if (session.phase === "observing" || session.phase === "writing") {
                    boardSession.writeOnStop = session.phase === "writing";
                    boardSession.phase = "stopping";
                    publishSnapshot();
                } else if (!isBusy(session)) {
                    openResults(session, "session");
                }
                break;
            case "last": if (lastResult) openResults(lastResult, "last"); break;
            case "results": go("detail", focus); break;
        }
    }

    function back() {
        switch (view) {
            case "home": go("closed"); break;
            case "closed": break;
            case "point": go("points", pointIndex); break;
            case "remove": go("point", 1); break;
            case "address":
                go(draft.previousView, draft.previousView === "points" ? draft.index : 0);
                draft = undefined;
                break;
            case "channel": draft.error = ""; go("address"); break;
            case "results": go(resultOrigin); break;
            case "detail": go("results", focus); break;
            case "file": go("detail", focus); break;
            default: go("home");
        }
    }

    function navigate(key) {
        if (view === "address") {
            if (key === "up") draft.row = Math.max(-1, draft.row - 1);
            if (key === "down") draft.row = Math.min(1, draft.row + 1);
            const delta = key === "right" ? 1 : key === "left" ? -1 : 0;
            if (draft.row === -1) draft.cursor = wrap(draft.cursor + delta, 12);
            else draft.col = wrap(draft.col + delta, 9);
        } else if (view === "channel" && (key === "left" || key === "right")) {
            draft.channel = wrap(draft.channel - 1 + (key === "right" ? 1 : -1), 13) + 1;
        } else if (view === "detail" && key === "right" && result.outcomes[focus] === "saved") {
            go("file", focus);
        } else if ((key === "up" || key === "down")) {
            const lengths = {
                home: 3, points: points.length + Number(points.length < 3), point: 2,
                results: result?.points.length,
            };
            const length = lengths[view];
            if (length) focus = wrap(focus + (key === "down" ? 1 : -1), length);
        }
    }

    function actionToken() {
        const page = screen();
        if (view === "address") return `hex:${draft.row}:${draft.col}:${draft.cursor}`;
        if (page.action) {
            const id = view === "session" ? session?.id : view === "last" ? lastResult?.id : "";
            return `${view}:${page.action}:${id}`;
        }
        const selected = page.rows?.find((row) => row.selected);
        return selected ? `${view}:${focus}:${selected.label}` : undefined;
    }

    function press(key) {
        if (key === "ok") {
            heldAction = actionToken();
        } else {
            heldAction = undefined;
            if (key === "back") {
                reopenPending = false;
                back();
            } else {
                const previousView = view;
                const previousFocus = focus;
                navigate(key);
                if (previousView !== view || previousFocus !== focus) reopenPending = false;
            }
        }
    }

    function release(key) {
        if (key !== "ok") return;
        const action = heldAction;
        heldAction = undefined;
        if (action && action === actionToken()) {
            reopenPending = false;
            ok();
        }
    }

    return {
        screen, press, release, step, toggleLink, reset,
        cancelPress() { heldAction = undefined; },
        board: () => ({ link, next: nextEvent() }),
    };
}

if (typeof module !== "undefined") module.exports = { createPrototype };
