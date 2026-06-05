#!/usr/bin/env python3
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional

APP_DIR = os.path.dirname(os.path.abspath(__file__))


@dataclass
class ProcSpec:
    name: str
    cmd: List[str]
    nice: int = 0
    cores_env: str = ""


@dataclass
class ProcState:
    spec: ProcSpec
    proc: Optional[subprocess.Popen] = None
    restart_count: int = 0
    last_start: float = 0.0
    exit_code: int = 0
    next_restart: float = 0.0

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None


def env_enabled(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value not in ("0", "false", "False", "FALSE", "none")


def child_setup(nice_adjust: int, cores_env: str):
    os.setsid()
    if cores_env:
        value = os.environ.get(cores_env, "")
        if value:
            try:
                cores = {int(part) for part in value.split(",") if part.strip()}
                if cores:
                    os.sched_setaffinity(0, cores)
            except (AttributeError, OSError, ValueError):
                pass
    if nice_adjust:
        try:
            os.nice(nice_adjust)
        except OSError:
            pass


class RpiManager:
    def __init__(self, argv: List[str]):
        if len(argv) != 3:
            raise ValueError(f"Usage: {argv[0] if argv else 'rpi_manager.py'} <model.param> <model.bin>")
        self.model_param = argv[1]
        self.model_bin = argv[2]
        self.shutdown = False
        self.procs: Dict[str, ProcState] = {}
        self.process_order = ["rpi_camerad", "rpi_modeld"]
        if env_enabled("RPI_RUN_OVERLAY", True):
            self.process_order.append("rpi_overlay")

        os.environ.setdefault("RPI_DISPLAY", "0")
        specs = [
            ProcSpec("rpi_camerad", [os.path.join(APP_DIR, "rpi_camerad")],
                     int(os.environ.get("RPI_CAMERAD_NICE", "5")), "RPI_CAMERAD_CORES"),
            ProcSpec("rpi_modeld", [os.path.join(APP_DIR, "rpi_modeld"), self.model_param, self.model_bin],
                     int(os.environ.get("RPI_MODELD_NICE", "0")), "RPI_MODELD_CORES"),
        ]
        if "rpi_overlay" in self.process_order:
            os.environ.setdefault("RPI_OVERLAY_FPS", "10")
            specs.append(ProcSpec("rpi_overlay", [os.path.join(APP_DIR, "rpi_overlay")],
                                  int(os.environ.get("RPI_OVERLAY_NICE", "10")), "RPI_OVERLAY_CORES"))

        for spec in specs:
            self.procs[spec.name] = ProcState(spec=spec)

    def cleanup_shm(self):
        if not env_enabled("RPI_CLEAR_SHM", True):
            return
        for name in (
            "/dev/shm/k230_road_ai",
            "/dev/shm/k230_road_ai_frame",
            "/dev/shm/k230_model_state",
            "/dev/shm/k230_manager_state",
        ):
            try:
                os.unlink(name)
            except FileNotFoundError:
                pass
            except OSError as exc:
                print(f"rpi_manager: failed to remove {name}: {exc}", flush=True)

    def start_proc(self, state: ProcState):
        if not os.path.exists(state.spec.cmd[0]):
            raise FileNotFoundError(f"{state.spec.cmd[0]} not found")
        state.proc = subprocess.Popen(
            state.spec.cmd,
            preexec_fn=lambda s=state.spec: child_setup(s.nice, s.cores_env),
        )
        state.last_start = time.monotonic()
        state.exit_code = 0
        print(
            f"rpi_manager: started {state.spec.name} pid={state.proc.pid} "
            f"nice={state.spec.nice} cmd={' '.join(state.spec.cmd)}",
            flush=True,
        )

    def terminate_proc(self, state: ProcState, sig=signal.SIGTERM):
        if not state.running():
            return
        try:
            os.killpg(os.getpgid(state.proc.pid), sig)
        except ProcessLookupError:
            pass

    def handle_signal(self, signum, _frame):
        print(f"\nrpi_manager: signal {signum}, stopping children", flush=True)
        self.shutdown = True
        for name in reversed(self.process_order):
            self.terminate_proc(self.procs[name], signal.SIGTERM)

    def run(self) -> int:
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, self.handle_signal)
        self.cleanup_shm()

        for name in self.process_order:
            self.start_proc(self.procs[name])
            time.sleep(0.25)

        max_sec = float(os.environ.get("RPI_MANAGER_MAX_SEC", "0") or "0")
        deadline = time.monotonic() + max_sec if max_sec > 0 else None
        exit_code = 0
        while not self.shutdown:
            now = time.monotonic()
            if deadline is not None and now >= deadline:
                print("\nrpi_manager: max runtime reached", flush=True)
                self.shutdown = True
                break

            for state in self.procs.values():
                if state.proc is None:
                    continue
                ret = state.proc.poll()
                if ret is None:
                    continue
                state.exit_code = ret
                state.proc = None
                exit_code = ret if ret != 0 else exit_code
                state.restart_count += 1
                state.next_restart = now + 1.0
                print(f"\nrpi_manager: {state.spec.name} exited code={ret}", flush=True)

            if env_enabled("RPI_NO_RESTART", False):
                if any(state.proc is None for state in self.procs.values()):
                    self.shutdown = True
                    break
            else:
                for state in self.procs.values():
                    if self.shutdown or state.proc is not None:
                        continue
                    if now >= state.next_restart:
                        self.start_proc(state)

            time.sleep(0.1)

        for name in reversed(self.process_order):
            self.terminate_proc(self.procs[name], signal.SIGTERM)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if all(not state.running() for state in self.procs.values()):
                break
            time.sleep(0.1)
        for name in reversed(self.process_order):
            self.terminate_proc(self.procs[name], signal.SIGKILL)
        return exit_code


def main(argv: List[str]) -> int:
    try:
        return RpiManager(argv).run()
    except Exception as exc:
        print(f"rpi_manager error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
