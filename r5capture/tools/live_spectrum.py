#!/usr/bin/env python3
"""Live spectrum and spectrograms of the R5's two microphones.

On the board:  sudo ./r5cap_ctl start 20000 1250000 irqoff ; sudo ./r5cap_stream
On the PC:     python3 live_spectrum.py [--host 192.168.7.2]
Keys: space = pause, c = clear. Move the mouse over a plot to read values.
Needs numpy, matplotlib and a GUI toolkit (PySide6, or Tk).
"""
import argparse
import collections
import socket
import struct
import sys
import threading
import time

import numpy as np
import matplotlib

PORT = 35200
MAGIC, VERSION = 0x54533552, 1           # include/r5cap_stream.h
HEADER, BLOCK = struct.Struct("<8I"), struct.Struct("<4I")
FLAG_LATE, FLAG_ADC_ERROR = 1, 2
FULL_SCALE = 2048.0                      # counts; 0 dBFS = a sine filling the ADC's range
ADC_MAX = 4095
FLOOR_DB = -110.0
RECONNECT_S = 1.0
CONNECT_TIMEOUT_S = 5.0
CLIP_HOLD_S = 1.0

SURFACE, PAGE, INK, INK2, MUTED = "#1a1a19", "#0d0d0d", "#f0efec", "#c3c2b7", "#898781"
GRID, AXIS = "#2c2c2a", "#383835"
MIC_COLORS = ("#3987e5", "#d95926")
RAMP = (SURFACE, "#0d366b", "#184f95", "#256abf", "#3987e5", "#86b6ef", "#cde2fb")


class Receiver(threading.Thread):
    """Reads the stream, reconnecting as needed. Shared state is guarded by lock."""

    def __init__(self, host, port, fft, hop):
        super().__init__(daemon=True)
        self.address, self.fft, self.hop = (host, port), fft, hop
        self.lock = threading.Lock()
        self.status, self.rate, self.offset_us = "connecting", None, 0.0
        self.blocks = self.skipped = self.late = self.adc_errors = 0
        self.mean, self.sd, self.clip_until = [0.0, 0.0], [0.0, 0.0], [0.0, 0.0]
        self.frames = collections.deque(maxlen=1000)   # FFT frames not drawn yet, in dBFS
        self.epoch = 0                                 # +1 whenever the settings may have changed
        self.window = np.hanning(fft)

    def run(self):
        while True:
            try:
                with socket.create_connection(self.address, CONNECT_TIMEOUT_S) as sock:
                    self.set_status("connected, waiting for the R5 to sample")
                    sock.settimeout(None)
                    self.receive(sock)
            except (OSError, EOFError) as e:
                self.set_status(f"no data from {self.address[0]}: {e or 'disconnected'} (retrying)")
            except ValueError as e:
                self.set_status(str(e))
            time.sleep(RECONNECT_S)

    def set_status(self, text):
        with self.lock:
            self.status = text

    @staticmethod
    def read(sock, n):
        buf = bytearray()
        while len(buf) < n:
            part = sock.recv(n - len(buf))
            if not part:
                raise EOFError("stream ended (R5 stopped or restarted?)")
            buf += part
        return bytes(buf)

    def receive(self, sock):
        magic, version, channels, frames, tick_hz, period, offset, _ = HEADER.unpack(self.read(sock, HEADER.size))
        if magic != MAGIC or version != VERSION:
            raise ValueError("not an r5cap_stream server, or a different version")
        if channels != 2 or not 0 < frames <= 4096 or not tick_hz or not period:
            raise ValueError(f"unsupported stream: {channels} channels, {frames} frames per block")
        with self.lock:
            self.rate, self.offset_us = tick_hz / period, offset * 1e6 / tick_hz
            self.status = "live"
            self.blocks = self.skipped = self.late = self.adc_errors = 0
            self.frames.clear()
            self.epoch += 1
        pending = np.empty((0, 2))
        while True:
            _, flags, skipped, _ = BLOCK.unpack(self.read(sock, BLOCK.size))
            data = np.frombuffer(self.read(sock, frames * channels * 2), "<u2").reshape(-1, 2).astype(float)
            if skipped:
                pending = np.empty((0, 2))   # never join samples across a gap
            pending = np.concatenate((pending, data))
            spectra = []
            while len(pending) >= self.fft:
                x = pending[:self.fft]
                spectra.append((self.spectrum(x), x.mean(0), x.std(0)))
                pending = pending[self.hop:]
            now = time.monotonic()
            clipped = ((data <= 0) | (data >= ADC_MAX)).any(0)
            with self.lock:
                self.blocks += 1
                self.skipped += skipped
                self.late += bool(flags & FLAG_LATE)
                self.adc_errors += bool(flags & FLAG_ADC_ERROR)
                for c in range(2):
                    if clipped[c]:
                        self.clip_until[c] = now + CLIP_HOLD_S
                for db, mean, sd in spectra:
                    self.frames.append(db)
                    self.mean, self.sd = list(mean), list(sd)

    def spectrum(self, x):
        mag = np.abs(np.fft.rfft((x - x.mean(0)) * self.window[:, None], axis=0))
        return 20 * np.log10(np.maximum(mag * 2 / self.window.sum() / FULL_SCALE, 1e-9))


class View:
    def __init__(self, plt, rx, args):
        from matplotlib.colors import LinearSegmentedColormap
        self.plt, self.rx, self.args = plt, rx, args
        self.epoch, self.paused, self.smooth, self.image = -1, False, None, [None, None]
        plt.rcParams.update({"font.size": 10, "text.color": INK, "axes.labelcolor": INK2,
                             "xtick.color": MUTED, "ytick.color": MUTED, "axes.edgecolor": AXIS,
                             "axes.facecolor": SURFACE, "figure.facecolor": PAGE,
                             "keymap.save": ["ctrl+s"], "keymap.home": ["h"], "keymap.back": ["left"]})
        self.fig = plt.figure(figsize=(12, 8), num="R5 live spectrum")
        grid = self.fig.add_gridspec(2, 3, width_ratios=(1, 1, 0.025), height_ratios=(1.15, 1),
                                     left=0.07, right=0.93, top=0.88, bottom=0.08, hspace=0.35, wspace=0.18)
        self.ax = self.fig.add_subplot(grid[0, :2])
        self.sg = [self.fig.add_subplot(grid[1, c]) for c in range(2)]
        self.cmap = LinearSegmentedColormap.from_list("blue", RAMP)
        self.cmap.set_bad(SURFACE)
        self.title = self.fig.text(0.07, 0.955, "", fontsize=12, color=INK)
        self.info = self.fig.text(0.07, 0.918, "", fontsize=10, color=INK2)
        self.readout = self.fig.text(0.93, 0.918, "", fontsize=10, color=INK2, ha="right")

        self.lines = [self.ax.plot([], [], color=MIC_COLORS[c], lw=2, solid_joinstyle="round",
                                   label=f"Mic {c}")[0] for c in range(2)]
        self.ax.set(xlabel="Frequency (Hz)", ylabel="Level (dBFS)", ylim=(FLOOR_DB, 0),
                    xscale="log" if args.log else "linear")
        self.ax.grid(color=GRID, lw=1)
        self.ax.set_axisbelow(True)
        self.ax.legend(loc="upper right", frameon=False, labelcolor=INK2)
        self.im = []
        for c, ax in enumerate(self.sg):
            self.im.append(ax.imshow(np.full((2, 2), np.nan), origin="lower", aspect="auto", cmap=self.cmap,
                                     vmin=args.min_db, vmax=args.max_db, interpolation="nearest"))
            ax.set(xlabel="Time (s, 0 = now)", title=f"Mic {c} spectrogram")
            ax.title.set(color=INK2, size=10)
        self.sg[0].set_ylabel("Frequency (Hz)")
        self.sg[1].tick_params(labelleft=False)
        bar = self.fig.colorbar(self.im[0], cax=self.fig.add_subplot(grid[1, 2]))
        bar.set_label("dBFS", color=INK2)
        bar.outline.set_edgecolor(AXIS)
        for ax in (self.ax, *self.sg):
            for side in ("top", "right"):
                ax.spines[side].set_visible(False)
        self.cursor = self.ax.axvline(100, color=MUTED, lw=1, visible=False)
        self.fig.canvas.mpl_connect("key_press_event", self.on_key)
        self.fig.canvas.mpl_connect("motion_notify_event", self.on_move)
        self.fig.canvas.mpl_connect("axes_leave_event", self.on_leave)

    def on_key(self, event):
        if event.key == " ":
            self.paused = not self.paused
        elif event.key == "c":
            self.epoch = -1

    def on_leave(self, _event):
        self.readout.set_text("")
        self.cursor.set_visible(False)

    def on_move(self, event):
        if event.inaxes is None or self.smooth is None or self.rx.rate is None:
            return self.on_leave(event)
        if event.inaxes is self.ax:
            k = int(np.clip(round(event.xdata / self.bin_hz), 0, len(self.smooth) - 1))
            self.cursor.set_xdata([k * self.bin_hz] * 2)
            self.cursor.set_visible(True)
            self.readout.set_text(f"{k * self.bin_hz:,.0f} Hz    mic 0 {self.smooth[k, 0]:.1f} dBFS    "
                                  f"mic 1 {self.smooth[k, 1]:.1f} dBFS")
        elif event.inaxes in self.sg:
            c = self.sg.index(event.inaxes)
            rows, cols = self.image[c].shape
            row = int(np.clip(round(event.ydata / self.bin_hz), 0, rows - 1))
            col = int(np.clip(cols + event.xdata / self.col_s, 0, cols - 1))
            value = self.image[c][row, col]
            level = "no data" if np.isnan(value) else f"{value:.1f} dBFS"
            self.readout.set_text(f"mic {c}    {row * self.bin_hz:,.0f} Hz    {-event.xdata:.1f} s ago    {level}")

    def reset(self, rate, epoch):
        self.epoch, self.smooth = epoch, None
        self.bin_hz, self.col_s = rate / self.args.fft, self.args.hop / rate
        bins, cols = self.args.fft // 2 + 1, max(2, int(self.args.seconds / self.col_s))
        for c in range(2):
            self.image[c] = np.full((bins, cols), np.nan)
            self.im[c].set_extent((-cols * self.col_s, 0, -self.bin_hz / 2, rate / 2 + self.bin_hz / 2))
            self.sg[c].set(xlim=(-cols * self.col_s, 0), ylim=(0, rate / 2))
        top = rate / 2
        self.ax.set_xlim(max(self.bin_hz * 2, 20) if self.args.log else 0, top)

    def update(self, _frame=None):
        rx = self.rx
        with rx.lock:
            rate, epoch, status, offset = rx.rate, rx.epoch, rx.status, rx.offset_us
            counts = (rx.blocks, rx.skipped, rx.late, rx.adc_errors)
            mean, sd, clip = list(rx.mean), list(rx.sd), list(rx.clip_until)
            frames = [] if self.paused else list(rx.frames)
            if not self.paused:
                rx.frames.clear()
        self.title.set_text("R5 microphones" + ("  (paused)" if self.paused else "") +
                            ("" if status == "live" else f"  -  {status}"))
        if rate is None:
            return
        if epoch != self.epoch:
            self.reset(rate, epoch)
        now = time.monotonic()
        mics = "    ".join(f"mic {c}: DC {mean[c]:.0f}, sd {sd[c]:.0f}" + ("  CLIPPING" if clip[c] > now else "")
                           for c in range(2))
        self.info.set_text(f"{rate:,.0f} samples/s    {mics}    skipped {counts[1]}    late {counts[2]}    "
                           f"ADC errors {counts[3]}    mic 1 lag {offset:.2f} µs")
        if not frames:
            return
        for db in frames:
            # Average power, not dB, so noise reads correctly
            power = 10 ** (db / 10)
            if self.smooth is None:
                smooth = power
            else:
                old = 10 ** (self.smooth / 10)
                smooth = old + (power - old) * self.args.alpha
            self.smooth = 10 * np.log10(np.maximum(smooth, 1e-18))
        new = np.stack(frames[-self.image[0].shape[1]:], axis=2)   # bins x mics x time
        n = new.shape[2]
        x = np.arange(len(self.smooth)) * self.bin_hz
        for c in range(2):
            self.image[c] = np.concatenate((self.image[c][:, n:], new[:, c, :]), axis=1)
            self.im[c].set_data(self.image[c])
            self.lines[c].set_data(x[1:], self.smooth[1:, c])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="192.168.7.2")
    p.add_argument("--port", type=int, default=PORT)
    p.add_argument("--fft", type=int, default=1024, help="FFT length in samples (default 1024)")
    p.add_argument("--hop", type=int, default=512, help="samples between FFTs (default 512)")
    p.add_argument("--seconds", type=float, default=10, help="spectrogram length (default 10)")
    p.add_argument("--alpha", type=float, default=0.25, help="spectrum smoothing, 1 = none (default 0.25)")
    p.add_argument("--min-db", type=float, default=-100)
    p.add_argument("--max-db", type=float, default=-20)
    p.add_argument("--log", action="store_true", help="logarithmic frequency axis on the spectrum")
    p.add_argument("--save", metavar="PNG", help="run without a window for --save-after seconds and save a picture")
    p.add_argument("--save-after", type=float, default=6)
    args = p.parse_args()
    if args.fft < 64 or not 0 < args.hop <= args.fft or not 0 < args.alpha <= 1 or args.seconds <= 0 \
            or args.min_db >= args.max_db:
        p.error("need fft >= 64, 0 < hop <= fft, 0 < alpha <= 1, seconds > 0, min-db < max-db")
    if args.save:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    rx = Receiver(args.host, args.port, args.fft, args.hop)
    rx.start()
    view = View(plt, rx, args)
    if args.save:
        end = time.monotonic() + args.save_after
        while time.monotonic() < end:
            view.update()
            time.sleep(0.05)
        view.fig.savefig(args.save, dpi=100)
        print(rx.status, "blocks", rx.blocks, "skipped", rx.skipped, "rate", rx.rate)
        return 0 if rx.blocks else 1
    if matplotlib.get_backend().lower() == "agg":
        sys.exit("no GUI toolkit for matplotlib: pip install PySide6-Essentials (or use --save)")
    from matplotlib.animation import FuncAnimation
    animation = FuncAnimation(view.fig, view.update, interval=50, cache_frame_data=False)
    plt.show()
    return 0 if animation else 1


if __name__ == "__main__":
    sys.exit(main())
