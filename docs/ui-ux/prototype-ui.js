"use strict";

const storageKey = "airrounds-prototype-profile";
const byId = (id) => document.getElementById(id);
const lcd = byId("lcd");
const context = lcd.getContext("2d");
const screenDescription = byId("screen-description");
const linkToggle = byId("link-toggle");
const linkState = byId("link-state");
const stepButton = byId("step");
const keyButtons = new Map([...document.querySelectorAll("[data-key]")]
    .map((button) => [button.dataset.key, button]));

function loadProfile() {
    try {
        const points = JSON.parse(localStorage.getItem(storageKey));
        if (Array.isArray(points) && points.length <= 3 && points.every((point) =>
            typeof point?.address === "string" && /^[0-9A-F]{12}$/.test(point.address) &&
            Number.isInteger(point.channel) && point.channel >= 1 && point.channel <= 13) &&
            new Set(points.map((point) => point.address)).size === points.length) return points;
    } catch {
        // The first launch uses the demo profile when the browser has no saved data.
    }
    return undefined;
}

const app = createPrototype({
    profile: loadProfile(),
    saveProfile(points) {
        localStorage.setItem(storageKey, JSON.stringify(points));
    },
});

const lcdPaper = "#f4b452";
const lcdInk = "#2c2416";
const glyph = (character) => lcdGlyphs[character.charCodeAt(0) - 32] ?? lcdGlyphs[31];

function textWidth(label) {
    return [...label].reduce((width, character) => width + glyph(character).length + 1, -1);
}

function text(label, x, baseline, { reverse = false, width = 124 - x } = {}) {
    const end = x + width;
    context.fillStyle = reverse ? lcdPaper : lcdInk;
    for (const character of label) {
        const columns = glyph(character);
        if (x + columns.length > end) return;
        columns.forEach((bits, column) => {
            for (let row = 0; row < 8; row++) {
                if (bits & (1 << row)) context.fillRect(x + column, baseline - 7 + row, 1, 1);
            }
        });
        x += columns.length + 1;
    }
}

function drawButton(action) {
    const width = textWidth(action) + 19;
    const x = Math.floor((128 - width) / 2);
    context.fillStyle = lcdInk;
    context.fillRect(x + 1, 52, width - 2, 12);
    context.fillRect(x, 54, width, 10);
    context.strokeStyle = lcdPaper;
    context.strokeRect(x + 4.5, 55.5, 5, 5);
    context.fillStyle = lcdPaper;
    context.fillRect(x + 7, 58, 1, 1);
    text(action, x + 14, 61, { reverse: true, width: width - 16 });
}

function drawMenu(page) {
    const top = page.subheading ? 26 : 17;
    if (page.subheading) text(page.subheading, 4, 22);
    page.rows.forEach((row, i) => {
        const y = top + i * (page.subheading ? 15 : 16);
        if (row.selected) {
            context.fillStyle = lcdInk;
            context.fillRect(2, y, 122, 13);
        }
        text(row.label, 5, y + 10, { reverse: row.selected, width: row.detail ? 54 : 115 });
        if (row.detail) text(row.detail, 65, y + 10, { reverse: row.selected, width: 55 });
    });
}

function drawHex({ address, cursor, row, col }) {
    for (let i = 0; i < address.length; i++) {
        const x = 5 + i * 8 + Math.floor(i / 2) * 2;
        text(address[i], x, 22);
        if (i === cursor) {
            context.strokeStyle = lcdInk;
            if (row < 0) context.strokeRect(x - .5, 14.5, 6, 9);
            else {
                context.fillStyle = lcdInk;
                context.fillRect(x, 24, 5, 1);
            }
        }
    }
    const labels = [...hexKeys.map((keys) => [...keys])];
    labels[0].push("Del");
    labels[1].push("Next");
    labels.forEach((keys, y) => keys.forEach((label, i) => {
        const selected = row === y && col === i;
        const x = i === 8 ? 100 : 4 + i * 12;
        const top = 31 + y * 15;
        if (selected) {
            context.fillStyle = lcdInk;
            context.fillRect(x - 2, top, i === 8 ? 25 : 10, 12);
        }
        text(label, x, top + 9, { reverse: selected });
    }));
}

function describe(page) {
    const rows = page.rows?.map((row) => [
        row.selected && "Selected:", row.label, row.detail,
    ].filter(Boolean).join(" "));
    const profile = page.profile?.map((point, i) =>
        `${pointName(i)}, ${formatAddress(point.address)}, channel ${point.channel}`);
    const hexSelection = page.hex && (page.hex.row < 0
        ? `Position ${page.hex.cursor + 1}`
        : `Selected: ${hexKeys[page.hex.row][page.hex.col] ?? (page.hex.row === 0 ? "Del" : "Next")}`);
    return [
        page.title, page.subheading, ...(rows ?? []), ...(page.lines ?? []), ...(profile ?? []),
        page.hex && `BSSID ${formatAddress(page.hex.address)}`, hexSelection, page.hint,
        page.action && `OK: ${page.action}`,
    ].filter(Boolean).join(". ");
}

function drawScreen(page) {
    context.clearRect(0, 0, 128, 64);
    context.fillStyle = lcdPaper;
    context.fillRect(0, 0, 128, 64);
    text(page.title, 4, 11);
    context.fillRect(4, 14, 120, 1);
    if (page.rows) drawMenu(page);
    if (page.hex) drawHex(page.hex);
    page.lines?.forEach((line, i) => text(line, 4, 25 + 11 * i));
    page.profile?.forEach((point, i) => {
        const y = 23 + 11 * i;
        text(String(i + 1), 4, y);
        text(formatAddress(point.address), 15, y, { width: 84 });
        text(`Ch ${point.channel}`, 102, y, { width: 24 });
    });
    if (page.hint) text(page.hint, 4, 61);
    if (page.action) drawButton(page.action);
    lcd.dataset.screen = page.title;
    const description = describe(page);
    if (screenDescription.textContent !== description) screenDescription.textContent = description;
}

function render() {
    drawScreen(app.screen());
    const { link, next } = app.board();
    const label = { ready: "Connected", offline: "Disconnected", checking: "Syncing" }[link];
    if (linkState.textContent !== label) linkState.textContent = label;
    linkState.dataset.link = link;
    linkToggle.textContent = link === "offline" ? "Reconnect UART" : "Disconnect UART";
    if (stepButton.textContent !== (next || "No pending event")) {
        stepButton.textContent = next || "No pending event";
        byId("event-state").textContent = next ? `Board event: ${next}` : "No pending board event";
    }
    stepButton.disabled = !next;
}

function press(key) {
    app.press(key);
    keyButtons.get(key).classList.add("pressed");
    render();
}

function release(key) {
    app.release(key);
    keyButtons.get(key).classList.remove("pressed");
    render();
}

function tap(key) {
    press(key);
    release(key);
}

const keyboard = new Map([
    ["ArrowUp", "up"], ["ArrowDown", "down"], ["ArrowLeft", "left"], ["ArrowRight", "right"],
    ["Enter", "ok"], ["Space", "ok"], ["Escape", "back"], ["Backspace", "back"],
]);
const pressedKeys = new Set();
document.addEventListener("keydown", (event) => {
    const key = keyboard.get(event.code);
    if (!key || event.altKey || event.ctrlKey || event.metaKey) return;
    if (key === "ok" && event.target instanceof HTMLButtonElement) return;
    event.preventDefault();
    if (event.repeat) return;
    pressedKeys.add(event.code);
    press(key);
});
document.addEventListener("keyup", (event) => {
    if (!pressedKeys.delete(event.code)) return;
    event.preventDefault();
    release(keyboard.get(event.code));
});
window.addEventListener("blur", () => {
    pressedKeys.clear();
    app.cancelPress();
    for (const button of keyButtons.values()) button.classList.remove("pressed");
});

for (const [key, button] of keyButtons) {
    let activePointer;
    button.addEventListener("pointerdown", (event) => {
        if (event.button !== 0 || activePointer !== undefined) return;
        event.preventDefault();
        button.setPointerCapture(event.pointerId);
        activePointer = event.pointerId;
        press(key);
    });
    button.addEventListener("pointerup", (event) => {
        if (activePointer !== event.pointerId) return;
        activePointer = undefined;
        const bounds = button.getBoundingClientRect();
        if (event.clientX < bounds.left || event.clientX > bounds.right ||
            event.clientY < bounds.top || event.clientY > bounds.bottom) {
            app.cancelPress();
        }
        release(key);
    });
    function cancelPointer() {
        if (activePointer === undefined) return;
        activePointer = undefined;
        app.cancelPress();
        button.classList.remove("pressed");
    }
    button.addEventListener("pointercancel", cancelPointer);
    button.addEventListener("lostpointercapture", cancelPointer);
    button.addEventListener("click", (event) => {
        if (event.detail === 0) tap(key);
    });
}

function simulatorButton(element, callback) {
    element.addEventListener("click", (event) => {
        if (event.detail > 0) element.blur();
        callback();
        render();
    });
}

simulatorButton(linkToggle, () => app.toggleLink());
simulatorButton(stepButton, () => app.step());
function loadPreset(demo) {
    const saved = app.reset(demo);
    const note = byId("storage-note");
    note.hidden = saved;
    note.textContent = saved ? "" : "The browser could not save the profile.";
}
simulatorButton(byId("demo-profile"), () => loadPreset(true));
simulatorButton(byId("empty-profile"), () => loadPreset(false));
render();
