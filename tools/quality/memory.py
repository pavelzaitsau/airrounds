"""Static memory budgets for the embedded builds.

The checks read compiler stack-usage reports. They count AirRounds frames only:
SDK, libc, and interrupt frames stay outside the measured path and use the
remaining stack. Device measurements are still required for the full stack.
"""

import re

# The Flipper app thread runs airrounds_app and every event-loop callback.
# Half of the thread stack is reserved for SDK, printf, and event-loop frames.
FLIPPER_OWN_STACK_SHARE = 2

# GCC cannot follow a call through a function pointer. Each indirect call site
# names its possible targets; an unlisted site fails the check.
FLIPPER_INDIRECT_TARGETS = {
    "SessionClient::sendFrame": ("App::send",),
}

# airrounds_uart globals and static buffers, in bytes of ESP32-S2 data RAM.
ESP32_GLOBAL_RAM_BUDGET = 64 * 1024
# The firmware runs on 8192-byte FreeRTOS task stacks. One AirRounds frame may
# use at most an eighth of that, which leaves room for the SD and Wi-Fi drivers.
ESP32_FRAME_LIMIT = 1024
# Reviewed frames above the limit, each with its own cap. writeSessionJournal
# holds the 2304-byte journal text and runs alone on the airrounds-log task.
ESP32_LARGE_FRAMES = {
    "writeSessionJournal": 2560,
}


def stack_size(manifest):
    match = re.search(r"stack_size\s*=\s*(\d+)", manifest.read_text())
    if match is None:
        raise SystemExit(f"No stack_size in {manifest}")
    return int(match.group(1))


def read_stack_usage(path):
    """Return {function: bytes}; fail on frames whose size depends on runtime data."""
    frames = {}
    for line in path.read_text().splitlines():
        location, size, qualifier = line.rsplit("\t", 2)
        function = location.split(":", 3)[-1]
        if qualifier != "static":
            raise SystemExit(f"Stack frame is {qualifier}, not static: {function}")
        frames[function] = max(frames.get(function, 0), int(size))
    return frames


def _callgraph(text):
    nodes, edges = {}, {}
    for title, label in re.findall(r'node:\s*\{\s*title:\s*"([^"]+)"\s*label:\s*"([^"]*)"', text):
        size = re.search(r"\\n(\d+) bytes \((\w+)", label)
        if size and size.group(2) != "static":
            raise SystemExit(f"Stack frame is {size.group(2)}, not static: {label}")
        nodes[title] = (int(size.group(1)) if size else 0, label.split("\\n", 1)[0], bool(size))
    for source, target in re.findall(r'edge:\s*\{\s*sourcename:\s*"([^"]+)"\s*targetname:\s*"([^"]+)"',
                                     text):
        edges.setdefault(source, set()).add(target)
    return nodes, edges


def _resolve_indirect(nodes, edges, targets):
    by_name = {}
    for title, (_, label, defined) in nodes.items():
        if defined:
            by_name.setdefault(re.sub(r"\(.*", "", label).split(" ")[-1], []).append(title)
    for source, callees in edges.items():
        if "__indirect_call" not in callees:
            continue
        label = nodes[source][1]
        site = next((name for name in targets if name + "(" in label), None)
        if site is None:
            raise SystemExit(f"Unlisted indirect call in {label}; add it to memory.py")
        callees.discard("__indirect_call")
        for target in targets[site]:
            matches = [title for name, titles in by_name.items() if name.endswith(target)
                       for title in titles]
            if not matches:
                raise SystemExit(f"Indirect call target {target} not found")
            callees.update(matches)


def flipper_stack(callgraph, manifest):
    """Check the worst AirRounds call path on the Flipper app thread; return its size."""
    nodes, edges = _callgraph(callgraph.read_text())
    _resolve_indirect(nodes, edges, FLIPPER_INDIRECT_TARGETS)
    defined = [title for title, (_, _, has_frame) in nodes.items() if has_frame]
    worst = {}

    def depth(node, path):
        if node in path:
            chain = " > ".join(nodes[step][1] for step in (*path, node))
            raise SystemExit(f"Recursion makes the stack unbounded: {chain}")
        if node not in worst:
            below = [depth(callee, (*path, node)) for callee in edges.get(node, ())]
            best = max(below, default=(0, ()))
            worst[node] = (nodes.get(node, (0,))[0] + best[0], (node, *best[1]))
        return worst[node]

    entry = next((title for title in defined if nodes[title][1].endswith("airrounds_app(void*)")),
                 None)
    if entry is None:
        raise SystemExit("airrounds_app not found in the Flipper call graph")
    total, path = depth(entry, ())
    # Any other function may be an event-loop callback on top of airrounds_app. Visiting
    # all of them also finds call cycles that no root reaches.
    for function in defined:
        if function != entry:
            callback = depth(function, ())
            if nodes[entry][0] + callback[0] > total:
                total, path = nodes[entry][0] + callback[0], (entry, *callback[1])
    budget = stack_size(manifest) // FLIPPER_OWN_STACK_SHARE
    chain = " > ".join(f"{re.sub(r'[(].*', '', nodes[step][1]).split('::')[-1]} {nodes[step][0]}"
                       for step in path if step in nodes)
    print(f"Flipper app stack: {total} of {budget} bytes for AirRounds frames; {chain}")
    if total > budget:
        raise SystemExit(f"Flipper app frames use {total} bytes; the budget is {budget}")
    return total


def frame_limit(function):
    name = re.sub(r"\(.*", "", function).split("::")[-1]
    return ESP32_LARGE_FRAMES.get(name, ESP32_FRAME_LIMIT)


def esp32_memory(build_output, stack_usage):
    """Check ESP32 global RAM and every stack frame in the sketch; return the RAM use."""
    match = re.search(r"Global variables use (\d+) bytes", build_output)
    if match is None:
        raise SystemExit("arduino-cli did not report global RAM use")
    globals_ = int(match.group(1))
    frames = read_stack_usage(stack_usage)
    over = [(function, size) for function, size in frames.items() if size > frame_limit(function)]
    largest = sorted(frames.items(), key=lambda item: item[1], reverse=True)[:3]
    print(f"ESP32 global RAM: {globals_} of {ESP32_GLOBAL_RAM_BUDGET} bytes; largest frames: " +
          "; ".join(f"{re.sub(r'[(].*', '', name).split(' ')[-1]} {size} of "
                    f"{frame_limit(name)}" for name, size in largest))
    if globals_ > ESP32_GLOBAL_RAM_BUDGET:
        raise SystemExit(f"ESP32 globals use {globals_} bytes; the budget is "
                         f"{ESP32_GLOBAL_RAM_BUDGET}")
    if over:
        function, size = over[0]
        raise SystemExit(f"{function} uses a {size}-byte frame; the limit is "
                         f"{frame_limit(function)}")
    return globals_
