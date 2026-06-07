#!/usr/bin/env python3
import argparse
import bz2
import sys
from collections import Counter
from pathlib import Path


def iter_events(openpilot_path: Path, rlog: Path):
    sys.path.insert(0, str(openpilot_path))
    from cereal import log  # noqa: WPS433

    return log.Event.read_multiple_bytes(bz2.decompress(rlog.read_bytes()))


def summarize(openpilot_path: Path, rlog: Path, max_events: int):
    event_counts = Counter()
    bus_counts = Counter()
    addr_counts = Counter()
    dlc_counts = Counter()
    returned_or_rejected = 0
    can_events = 0
    can_frames = 0
    sendcan_events = 0
    sendcan_frames = 0
    send_addr_counts = Counter()
    first_can_time = None
    last_can_time = None

    for event in iter_events(openpilot_path, rlog):
        try:
            which = event.which()
        except Exception:
            continue
        event_counts[which] += 1
        if which == "sendcan":
            sendcan_events += 1
            for msg in event.sendcan:
                sendcan_frames += 1
                send_addr_counts[(int(msg.src), int(msg.address))] += 1

        if which != "can":
            continue

        can_events += 1
        if first_can_time is None:
            first_can_time = int(event.logMonoTime)
        last_can_time = int(event.logMonoTime)

        for msg in event.can:
            src = int(msg.src)
            address = int(msg.address)
            dlc = len(bytes(msg.dat))
            can_frames += 1
            bus_counts[src] += 1
            addr_counts[(src, address)] += 1
            dlc_counts[dlc] += 1
            if src >= 128:
                returned_or_rejected += 1

        if max_events and can_events >= max_events:
            break

    duration_s = 0.0
    if first_can_time is not None and last_can_time is not None:
        duration_s = max(0.0, (last_can_time - first_can_time) * 1e-9)

    return {
        "path": rlog,
        "event_counts": event_counts,
        "can_events": can_events,
        "can_frames": can_frames,
        "sendcan_events": sendcan_events,
        "sendcan_frames": sendcan_frames,
        "duration_s": duration_s,
        "bus_counts": bus_counts,
        "addr_counts": addr_counts,
        "send_addr_counts": send_addr_counts,
        "dlc_counts": dlc_counts,
        "returned_or_rejected": returned_or_rejected,
    }


def format_counter(counter: Counter, limit: int = 12) -> str:
    return " ".join(f"{key}:{count}" for key, count in counter.most_common(limit)) or "-"


def format_addr_counter(counter: Counter, limit: int = 16) -> str:
    return " ".join(f"bus{bus}:{hex(addr)}x{count}"
                    for (bus, addr), count in counter.most_common(limit)) or "-"


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize openpilot rlog CAN traffic.")
    parser.add_argument("rlog", nargs="+", type=Path)
    parser.add_argument("--openpilot", type=Path, default=Path("/Users/chan/Documents/openpilot_c2"))
    parser.add_argument("--max-can-events", type=int, default=0,
                        help="Stop after this many CAN events per log. 0 means full log.")
    args = parser.parse_args()

    for path in args.rlog:
        result = summarize(args.openpilot, path, args.max_can_events)
        rate = result["can_frames"] / result["duration_s"] if result["duration_s"] > 0 else 0.0
        print(f"analyze_can_rlog: path={result['path']}")
        print(f"  can_events={result['can_events']} can_frames={result['can_frames']} "
              f"duration={result['duration_s']:.3f}s frame_rate={rate:.1f}/s "
              f"returned_or_rejected_src={result['returned_or_rejected']}")
        print(f"  sendcan_events={result['sendcan_events']} "
              f"sendcan_frames={result['sendcan_frames']} "
              f"send_top={format_addr_counter(result['send_addr_counts'])}")
        print(f"  buses={format_counter(result['bus_counts'])}")
        print(f"  dlc={format_counter(result['dlc_counts'])}")
        print(f"  top_addr={format_addr_counter(result['addr_counts'])}")
        print(f"  top_events={format_counter(result['event_counts'], 10)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
