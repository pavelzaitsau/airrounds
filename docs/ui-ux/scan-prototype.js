"use strict";

// State and logic of the scan-and-capture mockup (scan-prototype.html).
// The page only wires DOM events and draws what screen() describes.

const scanNetworks = ["TELEPORT", "Studio", "Guest", "Workshop", "Loft", "Atlas"];
const captureModes = [
    { id: "beacon", label: "Beacon" },
    { id: "eapol", label: "EAPOL" },
    { id: "todoB", label: "Todo B" },
];
const menuItems = [
    { id: "scan", label: "Scan area" },
    { id: "mode", label: "Capture mode" },
    { id: "flash", label: "Flash board hardware" },
    { id: "test", label: "Test board" },
    { id: "about", label: "About" },
];
const maxSelection = 5;
const listRows = 3;
const menuRows = 4;
const confirmedLinks = new Set(["ready", "restarted"]);
const activeSessions = new Set(["running", "finalizing"]);
const activeScans = new Set(["running", "pending"]);

const realClock = {
    setTimeout: (action, delay) => setTimeout(action, delay),
    clearTimeout: (id) => clearTimeout(id),
};

// Options are test hooks; the page passes only onChange.
//   clock        { setTimeout(action, delay), clearTimeout(id) }
//   board        initial board state, as if the app reopened:
//                link: "ready" | "restarted" | "lost" | "offline" | "checking" | "update"
//                session: "idle" | "running" | "finalizing"
//                scan: "idle" | "running" | "pending"
//                pending: true while a Start or Stop waits for a reply, or a command is unconfirmed
//                captureModes: the board advertises the capture-mode capability
//                modes: undefined while the `modes` reply is pending, else the listed mode ids
//   implemented  mode ids the app implements
//   onChange     called after every state change, timers included
function createScanPrototype({
    clock = realClock,
    board: initialBoard = {},
    implemented = ["beacon"],
    onChange = () => {},
} = {}) {
    const networks = scanNetworks.slice();
    const board = {
        link: "ready",
        session: "idle",
        scan: "idle",
        pending: false,
        captureModes: true,
        modes: captureModes.map(({ id }) => id),
        ...initialBoard,
    };
    if (Array.isArray(board.modes)) board.modes = board.modes.slice();
    if (!board.captureModes) board.modes = undefined;

    const selected = new Set();
    const saved = new Set();
    const missed = new Set();
    let mode = "beacon";
    let view = "home";
    let menuCursor = 0;
    let menuOffset = 0;
    let modeFocus = 0;
    let unavailableHint = false;
    let cursor = 0;
    let offset = 0;
    let scanReturn = "home";
    let timer;
    let round;
    let limitHint = false;

    function after(delay, action) {
        clock.clearTimeout(timer);
        timer = clock.setTimeout(() => {
            action();
            onChange();
        }, delay);
    }

    function cancelTimer() {
        clock.clearTimeout(timer);
        timer = undefined;
    }

    // Board and modes

    const boardConfirmed = () => confirmedLinks.has(board.link) && !board.pending;
    const boardListsMode = (id) => board.captureModes && Array.isArray(board.modes) && board.modes.includes(id);
    const modeAvailable = (id) => implemented.includes(id) && (id === "beacon" || boardListsMode(id));
    const modePending = () => mode !== "beacon" && !boardListsMode(mode);

    function lockHint() {
        if (!boardConfirmed()) return "Board state unknown";
        if (activeSessions.has(board.session) || round) return "Session active";
        if (activeScans.has(board.scan) || view === "scanning") return "Scan active";
        return null;
    }

    function setMode(id) {
        if (id === mode) return;
        mode = id;
        saved.clear();
        missed.clear();
    }

    function applyModes(list) {
        if (!board.captureModes) return;
        board.modes = Array.isArray(list) ? list.slice() : undefined;
        if (board.modes && !board.modes.includes(mode)) setMode("beacon");
    }

    function setBoard(patch) {
        if ("link" in patch && patch.link !== board.link) board.modes = undefined;
        for (const key of ["link", "session", "scan", "pending", "captureModes"]) {
            if (key in patch) board[key] = patch[key];
        }
        if (!board.captureModes) {
            board.modes = undefined;
            setMode("beacon");
        } else if ("modes" in patch) applyModes(patch.modes);
        onChange();
    }

    function modesReply(list) {
        applyModes(list);
        onChange();
    }

    // Main menu

    function menuEntries() {
        return menuItems.map((item, index) =>
            index === 0 && activeSessions.has(board.session) ? { id: "session", label: "Session status" } : item);
    }

    // As in the app: focus wraps, and the last visible row follows the focus.
    function focusMenu(index) {
        menuCursor = (index + menuItems.length) % menuItems.length;
        menuOffset = menuCursor < menuRows ? 0 : menuCursor - menuRows + 1;
    }

    function openMenuItem(index) {
        focusMenu(index);
        const { id } = menuEntries()[index];
        if (id === "session") {
            // The app shows the running round; the mockup has one only after its own Start.
            if (round) view = "running";
        } else if (id === "scan") scan();
        else if (id === "mode") {
            view = "mode";
            modeFocus = captureModes.findIndex((item) => item.id === mode);
            unavailableHint = false;
        } else view = "placeholder";
    }

    // Capture mode screen

    function modeKey(key) {
        unavailableHint = false;
        if (key === "up" || key === "down") {
            modeFocus = (modeFocus + (key === "up" ? captureModes.length - 1 : 1)) % captureModes.length;
        } else if (key === "back") {
            view = "home";
        } else if (key === "ok") {
            if (lockHint()) return;
            const { id } = captureModes[modeFocus];
            if (!modeAvailable(id)) {
                unavailableHint = true;
                return;
            }
            setMode(id);
            view = "home";
            focusMenu(menuItems.findIndex((item) => item.id === "mode"));
        }
    }

    function chooseMode(index) {
        unavailableHint = false;
        modeFocus = index;
        modeKey("ok");
    }

    // Scan list and round

    function scan() {
        scanReturn = view;
        view = "scanning";
        after(950, () => {
            view = "list";
        });
    }

    // As in the app: the focused row is the last visible one once the list scrolls.
    function keepVisible() {
        offset = cursor < listRows ? 0 : cursor - listRows + 1;
    }

    function select(index) {
        cursor = index;
        // A click hits a visible row; scrolling then would move the list under the pointer.
        // The device has no pointer, so the next arrow key returns to the device's scroll rule.
        if (cursor < offset || cursor >= offset + listRows) keepVisible();
        limitHint = false;
        if (!saved.has(index)) {
            if (selected.has(index)) selected.delete(index);
            else if (selected.size < maxSelection) selected.add(index);
            else limitHint = true;
        }
    }

    function start() {
        if (!canStart()) return;
        round = { targets: [...selected].sort((a, b) => a - b), index: 0, savedCount: 0, phase: "listen", mode };
        view = "running";
        after(2300, beginSave);
    }

    function beginSave() {
        if (view !== "running" || round.phase !== "listen") return;
        round.phase = "save";
        after(500, () => finishAttempt(true));
    }

    function finishAttempt(success) {
        const target = round.targets[round.index];
        if (success) {
            saved.add(target);
            missed.delete(target);
            round.savedCount++;
        } else {
            missed.add(target);
        }
        selected.delete(target);
        round.index++;
        const complete = round.index === round.targets.length;
        if (view === "stopping") returnToList();
        else if (complete) {
            const firstMissed = round.targets.find((index) => missed.has(index));
            if (firstMissed !== undefined) {
                cursor = firstMissed;
                keepVisible();
            }
            round.phase = "done";
            after(1550, returnToList);
        } else {
            round.phase = "listen";
            after(9500, beginSave);
        }
    }

    function stop() {
        if (view !== "running") return;
        if (round.phase === "done") {
            returnToList();
            return;
        }
        view = "stopping";
        if (round.phase === "listen") after(350, returnToList);
    }

    function returnToList() {
        cancelTimer();
        view = "list";
        round = undefined;
        limitHint = false;
    }

    function handle(key) {
        if (view === "home") {
            if (key === "ok") openMenuItem(menuCursor);
            else if (key === "up" || key === "down") focusMenu(menuCursor + (key === "up" ? -1 : 1));
        } else if (view === "mode") {
            modeKey(key);
        } else if (view === "placeholder" && key === "back") {
            view = "home";
        } else if (view === "scanning" && key === "back") {
            cancelTimer();
            view = scanReturn;
        } else if (view === "list") {
            if (key === "scan" || key === "left") scan();
            else if (key === "right") start();
            else if (key === "ok") select(cursor);
            else if (key === "up" || key === "down") {
                cursor = (cursor + (key === "up" ? networks.length - 1 : 1)) % networks.length;
                keepVisible();
                limitHint = false;
            } else if (key === "back") {
                view = "home";
            }
        } else if (view === "running") {
            if (key === "ok") stop();
            else if (key === "advance") beginSave();
            else if (key === "miss" && round.phase === "listen") finishAttempt(false);
        }
    }

    // Plain description of the current screen

    // As in the app, Start needs a selection, a confirmed board with no round or scan, and a
    // confirmed mode.
    function canStart() {
        return selected.size > 0 && !modePending() && boardConfirmed() &&
            !activeSessions.has(board.session) && !activeScans.has(board.scan);
    }

    // The app lists Log failed too; the mockup has no session log to fail.
    function listHeading() {
        if (limitHint) return "Maximum 5";
        if (board.link !== "ready" && board.link !== "restarted") return "Board offline";
        if (modePending()) return "Mode pending";
        return selected.size + "/5 selected";
    }

    function screen() {
        const common = {
            view,
            mode,
            simulation: {
                visible: view === "running" && round.phase !== "done",
                enabled: view === "running" && round.phase === "listen",
            },
        };
        if (view === "home") {
            const entries = menuEntries();
            return {
                ...common,
                title: "AirRounds",
                items: entries.map(({ label }) => label),
                focus: menuCursor,
                offset: menuOffset,
                rows: entries.slice(menuOffset, menuOffset + menuRows)
                    .map(({ label }, row) => ({ index: menuOffset + row, label, focused: menuOffset + row === menuCursor })),
                above: menuOffset > 0,
                below: menuOffset + menuRows < entries.length,
            };
        }
        if (view === "mode") {
            const hint = lockHint() || (unavailableHint ? "Not available yet" : null);
            return {
                ...common,
                title: "Capture mode",
                rows: captureModes.map(({ id, label }, index) => ({
                    id,
                    label,
                    current: id === mode,
                    available: modeAvailable(id),
                    right: modeAvailable(id) ? undefined : "Soon",
                    focused: index === modeFocus,
                })),
                hint,
                locked: Boolean(lockHint()),
            };
        }
        if (view === "placeholder") return { ...common, title: menuEntries()[menuCursor].label };
        if (view === "scanning") return { ...common, title: "Scan area" };
        if (view === "list") {
            return {
                ...common,
                title: "Beacons",
                heading: listHeading(),
                rows: networks.map((label, index) => ({
                    index,
                    label,
                    focused: index === cursor,
                    selected: selected.has(index),
                    saved: saved.has(index),
                    missed: missed.has(index),
                })),
                offset,
                above: offset > 0,
                below: offset + listRows < networks.length,
                selectedCount: selected.size,
                start: canStart(),
                modePending: modePending(),
                limitHint,
            };
        }
        const { targets, index, phase, savedCount } = round;
        const last = index > 0 ? targets[index - 1] : undefined;
        return {
            ...common,
            title: "Capture",
            round: {
                mode: round.mode,
                phase,
                index,
                total: targets.length,
                savedCount,
                current: networks[targets[index]],
                last: last === undefined ? undefined : { label: networks[last], saved: saved.has(last) },
            },
        };
    }

    function state() {
        return {
            mode,
            selected: [...selected].sort((a, b) => a - b),
            saved: [...saved].sort((a, b) => a - b),
            missed: [...missed].sort((a, b) => a - b),
            networks: networks.slice(),
            board: { ...board, modes: board.modes && board.modes.slice() },
            available: captureModes.map(({ id }) => id).filter(modeAvailable),
        };
    }

    function changed(action) {
        return (...args) => {
            action(...args);
            onChange();
        };
    }

    return {
        screen,
        state,
        press: changed(handle),
        openMenuItem: changed((index) => {
            if (view === "home") openMenuItem(index);
        }),
        select: changed((index) => {
            if (view === "list") select(index);
        }),
        chooseMode: changed((index) => {
            if (view === "mode") chooseMode(index);
        }),
        setBoard,
        modesReply,
    };
}

if (typeof module !== "undefined") {
    module.exports = { createScanPrototype, scanNetworks, captureModes, menuItems };
}
