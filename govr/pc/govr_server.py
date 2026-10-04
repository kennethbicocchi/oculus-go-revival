#!/usr/bin/env python3
"""GoVR PC server: capture -> NVENC (GStreamer) -> TCP (adb reverse) -> Oculus Go client.

Protocol: docs/PROTOCOL.md. One client at a time; the client reconnects on its own.
"""
import argparse
import collections
import json
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
import tty

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstVideo", "1.0")
from gi.repository import Gst, GstVideo  # noqa: E402

PORT = 9950  # ALVR uses 9943/9944
VERSION = 1
HELLO_SERVER, VIDEO_CONFIG, VIDEO_FRAME, SCREEN, PING, AUDIO_CONFIG, AUDIO_FRAME, OSD, VR_POINTER = (
    1, 2, 3, 4, 5, 6, 7, 8, 9)
HELLO_CLIENT, FRAME_ACK, HEAD_POSE, PONG, REQUEST_IDR, CONTROL, AUDIO_ACK, GAMEPAD = (
    64, 65, 66, 67, 68, 69, 70, 71)
CODECS = {"h264": 0, "hevc": 1}
# CONTROL commands (client gamepad -> PC); same names are accepted on the control socket.
CTL_NAMES = {1: "recenter", 2: "bigger", 3: "smaller", 4: "closer", 5: "farther",
             6: "up", 7: "down", 8: "env", 9: "idr", 10: "reset", 11: "overlay"}
GO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPTURE_HELPER = os.path.join(GO_ROOT, "build", "kwin-capture", "govr-kwin-capture")
STATE_FILE = os.path.join(GO_ROOT, "state", "screen.json")
AUDIO_RATE, AUDIO_CHANNELS, AUDIO_FRAME_MS = 48000, 2, 10
SINK_NAME = "govr"
# Video layouts for --source url: SCREEN flag bit and the encoded size (the Go decodes ~4K30).
LAYOUT_FLAGS = {"flat": 0, "360": 8, "360tb": 16, "180": 32, "180sbs": 64}
LAYOUT_SIZE = {"flat": (1920, 1080), "360": (3840, 1920), "360tb": (2880, 2880),
               "180": (2880, 2880), "180sbs": (3840, 1920)}
LAYOUT_NAMES = {"flat": "Schermo piatto", "360": "360°", "360tb": "360° 3D (sopra/sotto)",
                "180": "180°", "180sbs": "180° 3D (affiancato)"}
LAYOUT_CYCLE = ["360", "360tb", "180", "180sbs", "flat"]
AUDIO_RESTORE_FILE = os.path.join(GO_ROOT, "state", "audio-restore.json")
CONTROL_SOCKET = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "govr.sock")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class ScreenState:
    """Curved-screen placement, owned by the PC and pushed to the client."""

    def __init__(self, radius=2.0, arc_deg=100.0, pitch_deg=0.0, show_env=True):
        self.radius, self.arc_deg, self.pitch_deg = radius, arc_deg, pitch_deg
        self.recenter_seq = 0
        self.show_env = show_env
        self.desktop_over_vr = False
        self.debug_projection = False
        self.layout = "flat"  # flat | 360 | 360tb | 180 | 180sbs

    def pack(self):
        flags = ((1 if self.show_env else 0) | (2 if self.desktop_over_vr else 0) |
                 (4 if self.debug_projection else 0) | LAYOUT_FLAGS.get(self.layout, 0))
        return struct.pack("<fffII", self.radius, self.arc_deg, self.pitch_deg,
                           self.recenter_seq, flags)

    def apply(self, cmd):
        """Apply a named control command. Returns True if the state changed."""
        if cmd == "recenter":
            self.recenter_seq += 1
        elif cmd == "bigger":
            self.arc_deg = min(170.0, self.arc_deg + 5)
        elif cmd == "smaller":
            self.arc_deg = max(20.0, self.arc_deg - 5)
        elif cmd == "closer":
            self.radius = max(0.5, round(self.radius - 0.25, 2))
        elif cmd == "farther":
            self.radius = min(10.0, round(self.radius + 0.25, 2))
        elif cmd == "up":
            self.pitch_deg = min(60.0, self.pitch_deg + 5)
        elif cmd == "down":
            self.pitch_deg = max(-60.0, self.pitch_deg - 5)
        elif cmd == "env":
            self.show_env = not self.show_env
        elif cmd == "overlay":
            self.desktop_over_vr = not self.desktop_over_vr
        elif cmd == "reset":
            self.radius, self.arc_deg, self.pitch_deg = 2.0, 100.0, 0.0
            self.recenter_seq += 1
        else:
            return False
        return True

    def save(self):
        os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
        with open(STATE_FILE, "w") as f:
            json.dump({"radius": self.radius, "arc_deg": self.arc_deg,
                       "pitch_deg": self.pitch_deg, "show_env": self.show_env}, f)

    def load(self):
        try:
            with open(STATE_FILE) as f:
                d = json.load(f)
            self.radius, self.arc_deg = d["radius"], d["arc_deg"]
            self.pitch_deg, self.show_env = d["pitch_deg"], d["show_env"]
        except (OSError, ValueError, KeyError):
            pass

    def describe(self):
        return (f"radius={self.radius:.2f}m arc={self.arc_deg:.0f}deg "
                f"pitch={self.pitch_deg:.0f}deg env={'on' if self.show_env else 'off'}")


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.reset()
        self.last_print = time.monotonic()

    def reset(self):
        self.frames = self.bytes = self.acks = 0
        self.latencies, self.decodes, self.on_go, self.pc_side = [], [], [], []
        self.audio_lat, self.audio_buf, self.audio_packets, self.audio_underruns = [], [], 0, 0

    def report(self, log_file=None):
        now = time.monotonic()
        dt = now - self.last_print
        if dt < 2.0:
            return
        with self.lock:
            lat = sorted(self.latencies)
            dec = sorted(self.decodes)
            line = (f"tx {self.frames / dt:5.1f} fps {self.bytes * 8 / dt / 1e6:6.2f} Mbit/s | "
                    f"acked {self.acks / dt:5.1f} fps")
            if lat:
                line += (f" | latency capture->decoded avg {sum(lat) / len(lat):5.1f} ms "
                         f"p50 {lat[len(lat) // 2]:5.1f} p95 {lat[int(len(lat) * .95)]:5.1f}")
            if dec:
                line += f" | go decode avg {sum(dec) / len(dec):5.1f} ms"
            if self.on_go:
                line += f" go recv->decoded {sum(self.on_go) / len(self.on_go):5.1f} ms"
            if self.pc_side:
                line += f" | pc capture->sent {sum(self.pc_side) / len(self.pc_side):5.1f} ms"
            if self.audio_lat:
                a = sorted(self.audio_lat)
                line += (f" || audio {self.audio_packets / dt:5.1f} pkt/s latency capture->queued "
                         f"p50 {a[len(a) // 2]:5.1f} ms buffered {sum(self.audio_buf) / len(self.audio_buf):4.1f} ms "
                         f"underruns {self.audio_underruns}")
                if lat:
                    line += f" A/V offset (audio-video, p50) {a[len(a) // 2] - lat[len(lat) // 2]:+5.1f} ms"
            self.reset()
        self.last_print = now
        log(line)
        if log_file:
            log_file.write(time.strftime("%H:%M:%S ") + line + "\n")
            log_file.flush()


class Client:
    """One connected Go client."""

    def __init__(self, sock, on_message):
        self.sock = sock
        self.send_lock = threading.Lock()
        self.alive = True
        self.on_message = on_message
        threading.Thread(target=self._rx, daemon=True).start()

    def send(self, mtype, payload=b""):
        hdr = struct.pack("<B3xI", mtype, len(payload))
        try:
            with self.send_lock:
                self.sock.sendall(hdr + payload if len(payload) < 4096 else hdr)
                if len(payload) >= 4096:
                    self.sock.sendall(payload)
            return True
        except OSError:
            self.close()
            return False

    def _read(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
        return bytes(buf)

    def _rx(self):
        try:
            while self.alive:
                mtype, length = struct.unpack("<B3xI", self._read(8))
                self.on_message(self, mtype, self._read(length) if length else b"")
        except (OSError, ConnectionError):
            pass
        self.close()

    def close(self):
        if self.alive:
            self.alive = False
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()


class AudioSink:
    """The `govr` PipeWire null sink: apps routed here play on the headset."""

    def __init__(self, make_default=False, mute_pc=False):
        self.module = None
        self.prev_default = None
        self.muted_sink = None
        self._recover()
        if not self._exists():
            r = subprocess.run(["pactl", "load-module", "module-null-sink", f"sink_name={SINK_NAME}",
                                "sink_properties=device.description=GoVR-Headset",
                                f"rate={AUDIO_RATE}", f"channels={AUDIO_CHANNELS}"],
                               capture_output=True, text=True)
            if r.returncode == 0:
                self.module = r.stdout.strip()
            log(f"audio: created sink '{SINK_NAME}' (module {self.module})")
        else:
            log(f"audio: using existing sink '{SINK_NAME}'")
        self.prev_default = self._pactl("get-default-sink")
        if make_default and self.prev_default != SINK_NAME:
            self._pactl("set-default-sink", SINK_NAME)
            for line in self._pactl("list", "short", "sink-inputs").splitlines():
                self._pactl("move-sink-input", line.split()[0], SINK_NAME)
            log("audio: GoVR is now the default output; existing streams moved")
        if mute_pc and self.prev_default and self.prev_default != SINK_NAME:
            self.muted_sink = self.prev_default
            self._pactl("set-sink-mute", self.muted_sink, "1")
            log(f"audio: muted PC output {self.muted_sink}")
        # Remember what to undo, so a crash or kill -9 can be repaired by the next run.
        os.makedirs(os.path.dirname(AUDIO_RESTORE_FILE), exist_ok=True)
        with open(AUDIO_RESTORE_FILE, "w") as f:
            json.dump({"default_sink": self.prev_default, "muted_sink": self.muted_sink,
                       "module": self.module}, f)

    def _recover(self):
        """Undo the audio changes of a previous run that did not exit cleanly."""
        try:
            with open(AUDIO_RESTORE_FILE) as f:
                d = json.load(f)
        except (OSError, ValueError):
            return
        log("audio: repairing audio settings left by a previous run")
        self.prev_default, self.muted_sink, self.module = (d.get("default_sink"),
                                                           d.get("muted_sink"), d.get("module"))
        self.close()
        self.prev_default = self.muted_sink = self.module = None

    @staticmethod
    def _pactl(*a):
        return subprocess.run(["pactl", *a], capture_output=True, text=True).stdout.strip()

    def _exists(self):
        return SINK_NAME in [l.split()[1] for l in self._pactl("list", "short", "sinks").splitlines()
                             if len(l.split()) > 1]

    def close(self):
        if self.muted_sink:
            self._pactl("set-sink-mute", self.muted_sink, "0")
        if self.prev_default and self.prev_default != SINK_NAME and \
                self._pactl("get-default-sink") == SINK_NAME:
            self._pactl("set-default-sink", self.prev_default)
            for line in self._pactl("list", "short", "sink-inputs").splitlines():
                self._pactl("move-sink-input", line.split()[0], self.prev_default)
        if self.module:
            self._pactl("unload-module", self.module)
            log(f"audio: removed sink '{SINK_NAME}'")
        self.module = None
        try:
            os.unlink(AUDIO_RESTORE_FILE)
        except OSError:
            pass


def repair_audio():
    """Undo audio changes of a run that was killed (kill -9, crash). Safe to call anytime."""
    if os.path.exists(AUDIO_RESTORE_FILE):
        sink = AudioSink.__new__(AudioSink)
        sink.module = sink.prev_default = sink.muted_sink = None
        sink._recover()


def build_audio_pipeline(args):
    if args.audio == "test":
        src = (f"audiotestsrc is-live=true wave=sine freq=440 volume=0.3 "
               f"samplesperbuffer={AUDIO_RATE * AUDIO_FRAME_MS // 1000} ! ")
    else:
        src = (f"pulsesrc device={SINK_NAME}.monitor do-timestamp=true "
               f"buffer-time={AUDIO_FRAME_MS * 2000} latency-time={AUDIO_FRAME_MS * 1000} "
               "client-name=GoVR ! ")
    return (src + "audioconvert ! audioresample ! "
            f"audio/x-raw,format=S16LE,rate={AUDIO_RATE},channels={AUDIO_CHANNELS} ! "
            f"opusenc name=aenc frame-size={AUDIO_FRAME_MS} bitrate={args.audio_bitrate} "
            "audio-type=restricted-lowdelay bitrate-type=cbr ! "
            "appsink name=asink emit-signals=true sync=false max-buffers=50 drop=true")


def build_pipeline(args):
    w, h, fps = args.width, args.height, args.fps
    if args.source == "test":
        src = (f"videotestsrc is-live=true pattern={args.pattern} ! "
               f"video/x-raw,width={w},height={h},framerate={fps}/1 ! "
               "timeoverlay font-desc=\"Sans 48\" halignment=center valignment=center ! "
               "clockoverlay font-desc=\"Sans 24\" time-format=\"%H:%M:%S\" ! ")
        scale = (f"videoconvert ! videoscale ! videorate ! "
                 f"video/x-raw,format=NV12,width={w},height={h},framerate={fps}/1 ! ")
    elif args.source == "url":
        # Real-time playback of a file/stream: video -> NVENC, audio -> the govr sink (and
        # from there through the normal Opus path). sync=true paces both on the clock.
        v, a = args.media_uris
        src = f"uridecodebin3 uri=\"{v}\" name=dv dv. ! queue max-size-time=500000000 ! "
        scale = (f"videoconvert ! videoscale ! videorate ! "
                 f"video/x-raw,format=NV12,width={w},height={h},framerate={fps}/1 ! ")
        audio_src = f"uridecodebin3 uri=\"{a}\" name=da da. " if a != v else "dv. "
        args.extra_branch = (f" {audio_src}! queue max-size-time=500000000 ! audioconvert ! "
                             f"audioresample ! pulsesink device={SINK_NAME} sync=true")
    elif args.source == "kwin":
        # KWin delivers frames only on damage; keepalive resends the last one so a new
        # client always gets a keyframe. NVENC takes BGRx directly (GPU colour conversion);
        # videoconvert is a passthrough unless KWin hands us another RGB layout.
        src = (f"pipewiresrc path={args.pw_node} do-timestamp=true keepalive-time=500 "
               "provide-clock=false ! ")
        scale = "videoconvert ! "
        if args.scale_to:
            scale += f"videoscale ! video/x-raw,width={w},height={h} ! "
        scale += "video/x-raw,format=BGRx ! "
    else:
        raise ValueError(args.source)
    enc_name = "nvh265enc" if args.codec == "hevc" else "nvh264enc"
    parse = "h265parse" if args.codec == "hevc" else "h264parse"
    caps = "video/x-h265" if args.codec == "hevc" else "video/x-h264"
    enc = (f"{enc_name} name=enc preset=p1 tune=ultra-low-latency zerolatency=true bframes=0 "
           f"rc-mode=cbr bitrate={args.bitrate} vbv-buffer-size={args.bitrate // fps * 2} "
           f"gop-size={fps * 4} repeat-sequence-header=true ! "
           f"{parse} config-interval=-1 ! {caps},stream-format=byte-stream,alignment=au ! "
           f"appsink name=sink emit-signals=true sync={'true' if args.source == 'url' else 'false'} "
           "max-buffers=4 drop=true")
    queue = "" if args.source == "url" else "queue max-size-buffers=1 leaky=downstream ! "
    desc = src + queue + scale + enc + getattr(args, "extra_branch", "")
    return desc


def resolve_media(url):
    """Return (video_uri, audio_uri) for a local file, a direct URL or a YouTube/etc. page."""
    if os.path.exists(url):
        u = "file://" + os.path.abspath(url)
        return u, u
    ytdlp = os.path.join(GO_ROOT, ".venv", "bin", "yt-dlp")
    if url.startswith("http") and os.path.exists(ytdlp):
        # YouTube serves 360 videos twice: DASH formats in EAC cubemap projection ("mesh") and
        # HLS formats in plain equirectangular. Prefer HLS. <=2160p30: the Go's decode budget.
        fmt = ("bv*[protocol^=m3u8][height<=2160][fps<=30]+ba/"
               "bv*[height<=2160][fps<=30]+ba/b[height<=2160]/bv*+ba/b")
        r = subprocess.run([ytdlp, "-g", "-f", fmt, url], capture_output=True, text=True)
        uris = [l for l in r.stdout.splitlines() if l.strip()]
        if r.returncode == 0 and uris:
            return uris[0], uris[-1]
        log("yt-dlp failed:", r.stderr.strip()[-300:])
    return url, url


class Server:
    def __init__(self, args):
        self.args = args
        self.client = None
        self.screen = ScreenState(args.radius, args.arc, 0.0, not args.no_env)
        if not args.no_state:
            self.screen.load()
        self.screen.debug_projection = args.debug_projection
        self.screen.layout = args.layout
        self.stats = Stats()
        self.capture = None
        self.audio_pipeline = None
        self.audio_sink = None
        self.pad = None
        self.vr_pointer = None
        self.player_paused = False
        self.osd_until = 0.0
        self.menu_open = False
        self.menu_index = 0
        repair_audio()
        if args.source == "url":
            args.media_uris = resolve_media(args.url)
            if args.layout in LAYOUT_SIZE and args.layout != "flat":
                args.width, args.height = LAYOUT_SIZE[args.layout]
                args.fps = 30   # ~4K30 decode budget on the Go
            if args.layout != "flat" and args.bitrate == 20000:
                args.bitrate = 40000
            if args.audio == "off":
                args.audio = "sink"  # url audio is played into the govr sink
        if args.audio == "sink":
            self.audio_sink = AudioSink(args.audio_default, args.mute_pc)
        if args.source == "kwin":
            self.start_capture()
        self.pipeline = None
        self.streaming = False
        self.log_file = open(args.log, "a") if args.log else None
        Gst.init(None)

    # ---- KWin capture helper
    def start_capture(self):
        cmd = [CAPTURE_HELPER]
        if self.args.virtual:
            cmd += ["--virtual", self.args.virtual, "--name", "GoVR"]
        elif self.args.output:
            cmd += ["--output", self.args.output]
        if self.args.no_cursor:
            cmd += ["--no-cursor"]
        if not os.path.exists(CAPTURE_HELPER):
            sys.exit("capture helper missing: run scripts/install-kwin-capture.sh")
        self.capture = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        size = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = self.capture.stdout.readline().strip()
            if not line:
                break
            if line.startswith("SIZE"):
                size = tuple(int(v) for v in line.split()[1:3])
            elif line.startswith("NODE"):
                self.args.pw_node = line.split()[1]
                break
            elif line.startswith("FAILED"):
                sys.exit("capture failed: " + line)
        if not self.args.pw_node:
            sys.exit("capture helper gave no PipeWire node")
        if self.args.virtual:
            size = tuple(int(v) for v in self.args.virtual.split("x"))
        if size and not self.args.scale_to:
            self.args.width, self.args.height = size
        log(f"KWin stream: node {self.args.pw_node}, {self.args.width}x{self.args.height}")

    # ---- encoder
    def start_pipeline(self):
        if self.pipeline:
            return
        desc = build_pipeline(self.args)
        log("pipeline:", desc)
        self.pipeline = Gst.parse_launch(desc)
        # Timestamps must be CLOCK_MONOTONIC for the latency stats (pipewiresrc would
        # otherwise provide its own clock).
        self.pipeline.use_clock(Gst.SystemClock.obtain())
        # Capture timestamps: GstVideoEncoder rewrites output PTS with an offset of about one
        # hour, so pair encoder input and output buffers (FIFO: no B-frames, no reordering)
        # and remember base_time + input PTS (CLOCK_MONOTONIC) per output PTS.
        self.capture_times = {}
        pending = collections.deque()
        enc = self.pipeline.get_by_name("enc")

        def enc_in(pad, info):
            b = info.get_buffer()
            if b.pts != Gst.CLOCK_TIME_NONE:
                pending.append(self.pipeline.get_base_time() + b.pts)
            return Gst.PadProbeReturn.OK

        def enc_out(pad, info):
            if pending:
                self.capture_times[info.get_buffer().pts] = pending.popleft()
                if len(self.capture_times) > 64:
                    self.capture_times.pop(next(iter(self.capture_times)))
            return Gst.PadProbeReturn.OK

        enc.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, enc_in)
        enc.get_static_pad("src").add_probe(Gst.PadProbeType.BUFFER, enc_out)
        sink = self.pipeline.get_by_name("sink")
        sink.connect("new-sample", self._on_sample)
        bus = self.pipeline.get_bus()
        threading.Thread(target=self._bus_loop, args=(bus,), daemon=True).start()
        self.pipeline.set_state(Gst.State.PLAYING)
        if self.client and self.client.alive and not self.audio_pipeline:
            self.start_audio()   # stop_pipeline() also stops audio (e.g. after a layout switch)
        if getattr(self, "pending_seek", None):
            pos, self.pending_seek = self.pending_seek, None
            threading.Thread(target=self._resume_at, args=(self.pipeline, pos), daemon=True).start()

    # ---- audio
    def start_audio(self):
        if self.args.audio == "off" or self.audio_pipeline:
            return
        self.client.send(AUDIO_CONFIG, struct.pack("<IIII", 0, AUDIO_RATE, AUDIO_CHANNELS,
                                                   AUDIO_FRAME_MS))
        desc = build_audio_pipeline(self.args)
        log("audio pipeline:", desc)
        self.audio_pipeline = Gst.parse_launch(desc)
        self.audio_pipeline.use_clock(Gst.SystemClock.obtain())
        self.audio_pipeline.get_by_name("asink").connect("new-sample", self._on_audio_sample)
        self.audio_pipeline.set_state(Gst.State.PLAYING)

    def stop_audio(self):
        if self.audio_pipeline:
            self.audio_pipeline.set_state(Gst.State.NULL)
            self.audio_pipeline = None

    def _on_audio_sample(self, sink):
        buf = sink.emit("pull-sample").get_buffer()
        client, pipe = self.client, self.audio_pipeline
        if not client or not client.alive or not pipe:
            return Gst.FlowReturn.OK
        now = time.monotonic_ns()
        t = pipe.get_base_time() + buf.pts if buf.pts != Gst.CLOCK_TIME_NONE else now
        if not 0 <= now - t < 2 * Gst.SECOND:  # encoder shifted timestamps: fall back
            t = now - AUDIO_FRAME_MS * Gst.MSECOND
        ok, info = buf.map(Gst.MapFlags.READ)
        if ok:
            client.send(AUDIO_FRAME, struct.pack("<Q", t) + bytes(info.data))
            buf.unmap(info)
            with self.stats.lock:
                self.stats.audio_packets += 1
        return Gst.FlowReturn.OK

    def stop_pipeline(self):
        self.stop_audio()
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None
        self.streaming = False

    def force_idr(self):
        if not self.pipeline:
            return
        enc = self.pipeline.get_by_name("enc")
        ev = GstVideo.video_event_new_upstream_force_key_unit(Gst.CLOCK_TIME_NONE, True, 0)
        enc.get_static_pad("src").send_event(ev)

    def _bus_loop(self, bus):
        pipe = self.pipeline
        while pipe is self.pipeline and pipe is not None:
            msg = bus.timed_pop_filtered(200 * Gst.MSECOND,
                                         Gst.MessageType.ERROR | Gst.MessageType.EOS |
                                         Gst.MessageType.WARNING)
            if not msg:
                continue
            if msg.type == Gst.MessageType.ERROR:
                err, dbg = msg.parse_error()
                log("GStreamer error:", err.message, dbg)
                os.kill(os.getpid(), signal.SIGTERM)  # unwind via cleanup
                return
            elif msg.type == Gst.MessageType.WARNING:
                w, dbg = msg.parse_warning()
                log("GStreamer warning:", w.message)
            else:
                log("EOS")
                if self.args.source == "url":
                    # Video finished: back to the desktop in the headset (as a new session).
                    subprocess.Popen([os.path.join(GO_ROOT, "scripts", "govr-launch"), "desktop"],
                                     start_new_session=True, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
                os.kill(os.getpid(), signal.SIGTERM)
                return

    def _on_sample(self, sink):
        sample = sink.emit("pull-sample")
        buf = sample.get_buffer()
        client = self.client
        if not client or not client.alive or not self.streaming:
            return Gst.FlowReturn.OK
        # Capture time on CLOCK_MONOTONIC: GstSystemClock is monotonic, so
        # base_time + pts (running time) is the absolute capture timestamp.
        t_capture = self.capture_times.pop(buf.pts, None) or time.monotonic_ns()
        ok, info = buf.map(Gst.MapFlags.READ)
        if not ok:
            return Gst.FlowReturn.OK
        data = bytes(info.data)
        buf.unmap(info)
        key = not buf.has_flags(Gst.BufferFlags.DELTA_UNIT)
        client.send(VIDEO_FRAME, struct.pack("<QI", t_capture, 1 if key else 0) + data)
        sent = time.monotonic_ns()
        with self.stats.lock:
            self.stats.pc_side.append((sent - t_capture) / 1e6)
            self.stats.frames += 1
            self.stats.bytes += len(data)
        return Gst.FlowReturn.OK

    # ---- client messages (client rx thread)
    def on_message(self, client, mtype, data):
        if mtype == HELLO_CLIENT:
            ver, ew, eh, hz = struct.unpack("<IIIf", data[:16])
            log(f"client hello: v{ver} eye {ew}x{eh} @ {hz:.0f} Hz")
            self.start_audio()
        elif mtype == REQUEST_IDR:
            log("client requests IDR (decoder ready)")
            self.streaming = True
            if self.pipeline:
                self.force_idr()
            else:
                self.start_pipeline()
        elif mtype == FRAME_ACK:
            t, dec_us, on_go_us = struct.unpack("<QII", data[:16])
            lat = (time.monotonic_ns() - t) / 1e6
            with self.stats.lock:
                self.stats.acks += 1
                if 0 < lat < 5000:
                    self.stats.latencies.append(lat)
                self.stats.decodes.append(dec_us / 1000)
                self.stats.on_go.append(on_go_us / 1000)
        elif mtype == AUDIO_ACK:
            t, buffered, underruns = struct.unpack("<QII", data[:16])
            with self.stats.lock:
                self.stats.audio_lat.append((time.monotonic_ns() - t) / 1e6)
                self.stats.audio_buf.append(buffered)
                self.stats.audio_underruns = underruns
        elif mtype == GAMEPAD:
            if self.pad is None and not self.args.no_gamepad:
                try:
                    from gamepad import VirtualPad
                    self.pad = VirtualPad()
                    log("gamepad: virtual Xbox 360 pad created (forwarded from the headset)")
                except Exception as ex:  # no evdev or no /dev/uinput access
                    log(f"gamepad: cannot create virtual pad: {ex}")
                    self.args.no_gamepad = True
            if self.pad:
                self.pad.update(data)
            if self.vr_pointer:
                self.vr_pointer.feed_headset_pad(data)
        elif mtype == CONTROL:
            cmd = struct.unpack("<I", data[:4])[0]
            self.control(CTL_NAMES.get(cmd, ""), source="gamepad")
        elif mtype == HEAD_POSE:
            self.last_pose = struct.unpack("<Qffff", data[:24])
        elif mtype == PONG:
            pass

    # ---- 360 / media player (source=url)
    PLAYER_CMDS = {"pause": None, "fwd": 5, "back": -5, "fwd30": 30, "back30": -30}
    MENU_ITEMS = [("resume", "▶  Riprendi"), ("format", "Formato: {fmt}  (A per cambiare)"),
                  ("back30", "⏪  −30 secondi"), ("fwd30", "⏩  +30 secondi"),
                  ("desktop", "🖥  Torna al desktop"), ("stop", "⏹  Chiudi tutto")]

    def _menu_text(self):
        lines = ["GoVR  ·  " + self._player_status(), ""]
        for i, (key, label) in enumerate(self.MENU_ITEMS):
            label = label.format(fmt=LAYOUT_NAMES.get(self.args.layout, self.args.layout))
            lines.append(("▸ " if i == self.menu_index else "   ") + label)
        return "\n".join(lines)

    def menu(self, cmd):
        """In-headset menu shown with the player OSD (Start on the pad / Meta+Alt+M)."""
        if cmd == "menu":
            self.menu_open = not self.menu_open
            self.menu_index = 0
        elif not self.menu_open:
            return False
        elif cmd == "menu_up":
            self.menu_index = (self.menu_index - 1) % len(self.MENU_ITEMS)
        elif cmd == "menu_down":
            self.menu_index = (self.menu_index + 1) % len(self.MENU_ITEMS)
        elif cmd == "menu_close":
            self.menu_open = False
        elif cmd == "menu_ok":
            key = self.MENU_ITEMS[self.menu_index][0]
            if key == "resume":
                self.menu_open = False
                if self.player_paused:
                    self.player("pause")
            elif key == "format":
                self.switch_layout("next")
            elif key in ("back30", "fwd30"):
                self.player(key)
            elif key in ("desktop", "stop"):
                self.menu_open = False
                self._osd_set("Chiusura…")
                subprocess.Popen([os.path.join(GO_ROOT, "scripts", "govr-launch"), key],
                                 start_new_session=True, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        self._osd_set(self._menu_text() if self.menu_open else "")
        return True

    def _osd_set(self, text):
        """Show text on the headset's head-locked 2D panel (empty hides it)."""
        if text == getattr(self, "_osd_last", None):
            return
        self._osd_last = text
        if self.client and self.client.alive:
            self.client.send(OSD, text.encode())

    def _player_status(self):
        if not self.pipeline:
            return ""
        ok_p, pos = self.pipeline.query_position(Gst.Format.TIME)
        ok_d, dur = self.pipeline.query_duration(Gst.Format.TIME)
        fmt = lambda t: f"{int(t // 60e9):02d}:{int(t // 1e9) % 60:02d}"
        if not ok_p:
            return ""
        bar = ""
        if ok_d and dur > 0:
            n = 16
            k = max(0, min(n, int(n * pos / dur)))
            bar = "  " + "█" * k + "░" * (n - k) + "  "
        return f"{fmt(pos)}{bar}{fmt(dur) if ok_d and dur > 0 else ''}"

    def _osd_loop(self):
        while True:
            time.sleep(0.5)
            if not self.pipeline or self.args.source != "url":
                continue
            if self.menu_open:
                self._osd_set(self._menu_text())
            elif self.player_paused:
                self._osd_set("⏸   " + self._player_status())
            elif time.monotonic() < self.osd_until:
                self._osd_set(getattr(self, "osd_prefix", "") + "▶   " + self._player_status())
            else:
                self.osd_prefix = ""
                self._osd_set("")

    def player(self, name):
        if self.args.source != "url" or not self.pipeline:
            return
        if name == "pause":
            if not self.player_paused:
                self.player_paused = True
                self._osd_set("⏸   " + self._player_status())
                self.pipeline.set_state(Gst.State.PAUSED)
            else:
                self.player_paused = False
                self.osd_until = time.monotonic() + 3
                self.pipeline.set_state(Gst.State.PLAYING)
            log("player:", "paused" if self.player_paused else "playing")
            return
        ok, pos = self.pipeline.query_position(Gst.Format.TIME)
        if not ok:
            return
        ok_d, dur = self.pipeline.query_duration(Gst.Format.TIME)
        target = max(0, pos + self.PLAYER_CMDS[name] * Gst.SECOND)
        if ok_d and dur > 0:
            target = min(target, dur - Gst.SECOND)
        self.pipeline.seek_simple(Gst.Format.TIME,
                                  Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE, target)
        self.osd_until = time.monotonic() + 3
        self.force_idr()
        log(f"player: seek {self.PLAYER_CMDS[name]:+d}s -> {target / 1e9:.0f}s")

    def _gamepad_player_loop(self):
        """While a video plays, the Xbox pad on the PC controls it (read-only, not grabbed)."""
        try:
            import evdev
            from evdev import ecodes as ec
        except ImportError:
            return
        while True:
            pads = [evdev.InputDevice(p) for p in evdev.list_devices()]
            pads = [d for d in pads if "xbox" in d.name.lower() and "govr" not in d.name.lower()]
            if not pads:
                time.sleep(3)
                continue
            dev = pads[0]
            log(f"player: gamepad controls on {dev.name} (A pause, LB/RB -/+5 s, "
                "D-pad -/+30 s, X format, Y recenter, Start/View menu)")
            try:
                for ev in dev.read_loop():
                    cmd = None
                    if ev.type == ec.EV_KEY and ev.value == 1:
                        cmd = {ec.BTN_SOUTH: "pause", ec.BTN_TL: "back", ec.BTN_TR: "fwd",
                               ec.BTN_WEST: "recenter", ec.BTN_NORTH: "layout",
                               ec.BTN_START: "menu", ec.BTN_SELECT: "menu", ec.BTN_MODE: "menu",
                               ec.KEY_MENU: "menu", ec.BTN_EAST: "menu_close"}.get(ev.code)
                    elif ev.type == ec.EV_ABS and ev.code == ec.ABS_HAT0X and ev.value:
                        cmd = "fwd30" if ev.value > 0 else "back30"
                    elif ev.type == ec.EV_ABS and ev.code == ec.ABS_HAT0Y and ev.value and self.menu_open:
                        cmd = "menu_down" if ev.value > 0 else "menu_up"
                    if cmd:
                        log(f"player: gamepad {cmd}")
                    if cmd:
                        try:
                            self.control(cmd, source="gamepad")
                        except Exception as ex:
                            log(f"gamepad '{cmd}' failed: {ex!r}")
            except OSError:
                time.sleep(2)  # pad disconnected: look again

    def _resume_at(self, pipe, pos):
        """Seek a freshly started pipeline once it is really playing (seeking during preroll
        is silently ignored by uridecodebin3)."""
        for _ in range(100):
            ok, cur = pipe.query_position(Gst.Format.TIME)
            if ok and cur > 0:
                break
            time.sleep(0.1)
        ok = pipe.seek_simple(Gst.Format.TIME, Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE, pos)
        self.force_idr()
        log(f"player: resumed at {pos / 1e9:.0f}s ({'ok' if ok else 'seek failed'})")

    def switch_layout(self, layout):
        """Change the projection of the playing video; restarts the encoder at the new size
        and resumes from the same position."""
        if self.args.source != "url" or not self.client or not self.client.alive:
            return
        if layout == "next":
            cur = self.args.layout if self.args.layout in LAYOUT_CYCLE else "flat"
            layout = LAYOUT_CYCLE[(LAYOUT_CYCLE.index(cur) + 1) % len(LAYOUT_CYCLE)]
        if layout not in LAYOUT_FLAGS:
            return
        pos = None
        if self.pipeline:
            ok, p = self.pipeline.query_position(Gst.Format.TIME)
            pos = p if ok else None
        log(f"player: layout {self.args.layout} -> {layout} at {pos / 1e9 if pos else 0:.0f}s")
        self.stop_pipeline()
        self.args.layout = layout
        self.args.width, self.args.height = LAYOUT_SIZE[layout]
        self.screen.layout = layout
        self.pending_seek = pos
        self.osd_until = time.monotonic() + 4
        self.osd_prefix = LAYOUT_NAMES[layout] + "   "
        self.client.send(VIDEO_CONFIG, struct.pack("<III", CODECS[self.args.codec],
                                                   self.args.width, self.args.height))
        self.push_screen()   # the client recreates its decoder and asks for a keyframe

    def control(self, name, source="key"):
        if self.args.source == "url":
            if name == "menu" or name.startswith("menu_"):
                self.menu(name)
                return
            if self.menu_open:   # reuse the player keys to navigate the menu
                mapped = {"back": "menu_up", "fwd": "menu_down", "pause": "menu_ok",
                          "back30": "menu_up", "fwd30": "menu_down", "recenter": "menu_close"}.get(name)
                if mapped:
                    self.menu(mapped)
                    return
        if name.startswith("layout"):
            self.switch_layout(name[len("layout"):] or "next")
            return
        if name in self.PLAYER_CMDS:
            self.player(name)
            return
        if name == "idr":
            self.force_idr()
            log("forced IDR")
            return
        if self.screen.apply(name):
            log(f"control '{name}' ({source})")
            self.screen.save()
            self.push_screen()

    def _control_socket_loop(self):
        try:
            os.unlink(CONTROL_SOCKET)
        except OSError:
            pass
        cs = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        cs.bind(CONTROL_SOCKET)
        while True:
            name = cs.recv(64).decode(errors="ignore").strip()
            try:
                self.control(name, source="govr-ctl")
            except Exception as ex:  # never let one bad command kill the control channel
                log(f"control '{name}' failed: {ex!r}")

    def _keyboard_loop(self):
        keys = {"f": "layout", " ": "pause", "j": "back", "l": "fwd", "J": "back30", "L": "fwd30",
                "r": "recenter", "+": "bigger", "=": "bigger", "-": "smaller",
                "[": "closer", "]": "farther", "w": "up", "s": "down", "e": "env",
                "i": "idr", "0": "reset", "o": "overlay"}
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        try:
            tty.setcbreak(fd)
            while True:
                if select.select([sys.stdin], [], [], 0.5)[0]:
                    c = sys.stdin.read(1)
                    if c == "q":
                        os.kill(os.getpid(), signal.SIGTERM)
                        return
                    if c in keys:
                        self.control(keys[c])
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)

    def _send_pointer(self, payload):
        c = self.client
        if c and c.alive:
            c.send(VR_POINTER, payload)

    def push_screen(self):
        if self.client and self.client.alive:
            self.client.send(SCREEN, self.screen.pack())
        log("screen:", self.screen.describe())

    # ---- main loop
    def serve(self):
        if not self.args.no_adb:
            r = subprocess.run(["adb", "reverse", f"tcp:{PORT}", f"tcp:{PORT}"],
                               capture_output=True, text=True)
            log("adb reverse:", (r.stdout + r.stderr).strip() or "ok")
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind(("127.0.0.1", PORT))
        ls.listen(1)
        ls.settimeout(0.5)
        threading.Thread(target=self._control_socket_loop, daemon=True).start()
        if self.args.vr_pointer:
            from vr_pointer import VrPointer
            self.vr_pointer = VrPointer(self._send_pointer, log, self._osd_set)
            self.vr_pointer.start()
        if self.args.source == "url":
            threading.Thread(target=self._osd_loop, daemon=True).start()
            threading.Thread(target=self._gamepad_player_loop, daemon=True).start()
        if sys.stdin.isatty():
            threading.Thread(target=self._keyboard_loop, daemon=True).start()
            log("keys: r=recenter +/-=size [/]=distance w/s=up/down e=environment "
                "0=reset i=keyframe q=quit")
        log(f"listening on 127.0.0.1:{PORT} (source={self.args.source}, codec={self.args.codec}, "
            f"{self.args.width}x{self.args.height}@{self.args.fps}, {self.args.bitrate} kbit/s)")
        deadline = time.monotonic() + self.args.duration if self.args.duration else None
        try:
            self._serve_loop(ls, deadline)
        finally:
            self.shutdown()

    def _serve_loop(self, ls, deadline):
        while deadline is None or time.monotonic() < deadline:
            try:
                sock, addr = ls.accept()
            except socket.timeout:
                self._tick()
                continue
            if self.client and self.client.alive:
                log("second client refused")
                sock.close()
                continue
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
            self.stop_pipeline()
            self.client = Client(sock, self.on_message)
            self._osd_last = None
            log("client connected")
            self.client.send(HELLO_SERVER, struct.pack("<I", VERSION))
            self.client.send(VIDEO_CONFIG, struct.pack("<III", CODECS[self.args.codec],
                                                       self.args.width, self.args.height))
            self.push_screen()

    def shutdown(self):
        """Always runs (normal exit, Ctrl+C, SIGTERM): restore the PC's audio setup."""
        self.stop_pipeline()
        if self.capture:
            self.capture.terminate()
        if self.audio_sink:
            self.audio_sink.close()
            self.audio_sink = None
        if self.pad:
            self.pad.close()
            self.pad = None

    def _ensure_adb_reverse(self):
        """ALVR runs `adb kill-server` when SteamVR stops, which drops our reverse tunnel."""
        now = time.monotonic()
        if self.args.no_adb_watch or now - getattr(self, "_last_adb_check", 0) < 3:
            return
        self._last_adb_check = now
        r = subprocess.run(["adb", "reverse", "--list"], capture_output=True, text=True)
        if f"tcp:{PORT}" not in r.stdout:
            subprocess.run(["adb", "reverse", f"tcp:{PORT}", f"tcp:{PORT}"], capture_output=True)
            log("adb reverse re-established")

    def _tick(self):
        self._ensure_adb_reverse()
        if self.client and not self.client.alive:
            log("client disconnected")
            self.client = None
            self.stop_pipeline()
        if self.client:
            self.stats.report(self.log_file)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", choices=["test", "kwin", "url"], default="test")
    p.add_argument("--url", help="--source url: file path, media URL or YouTube page")
    p.add_argument("--layout", choices=list(LAYOUT_FLAGS), default="flat",
                   help="how the headset shows the video: curved screen or 360 sphere")
    p.add_argument("--output", help="KWin output to mirror, e.g. DP-3 (default: first)")
    p.add_argument("--virtual", metavar="WxH", help="create a virtual monitor of this size")
    p.add_argument("--no-cursor", action="store_true")
    p.add_argument("--scale-to", action="store_true", help="scale capture to --width/--height")
    p.add_argument("--no-state", action="store_true", help="ignore saved screen placement")
    p.add_argument("--pattern", default="smpte")
    p.add_argument("--codec", choices=list(CODECS), default="hevc")
    p.add_argument("--width", type=int, default=1920)
    p.add_argument("--height", type=int, default=1080)
    p.add_argument("--fps", type=int, default=60)
    p.add_argument("--bitrate", type=int, default=20000, help="kbit/s")
    p.add_argument("--radius", type=float, default=2.0)
    p.add_argument("--arc", type=float, default=100.0)
    p.add_argument("--no-env", action="store_true")
    p.add_argument("--debug-projection", action="store_true",
                   help="show the stream through the SteamVR projection-layer path (test)")
    p.add_argument("--no-adb", action="store_true", help="do not set up adb reverse at start")
    p.add_argument("--no-adb-watch", action="store_true",
                   help="do not re-create the adb reverse tunnel if it disappears")
    p.add_argument("--audio", choices=["sink", "test", "off"], default="sink",
                   help="sink: stream the 'govr' virtual sink; test: 440 Hz tone")
    p.add_argument("--audio-bitrate", type=int, default=128000)
    p.add_argument("--audio-default", action="store_true",
                   help="make GoVR the default output while running (restored on exit)")
    p.add_argument("--mute-pc", action="store_true", help="mute the PC speakers while running")
    p.add_argument("--vr-pointer", action="store_true",
                   help="SteamVR session: the Xbox pad drives a laser pointer in the dashboard")
    p.add_argument("--no-gamepad", action="store_true",
                   help="ignore the headset gamepad (no virtual pad on the PC)")
    p.add_argument("--duration", type=float, default=0, help="exit after N seconds")
    p.add_argument("--log", help="append stats lines to this file")
    p.add_argument("--pw-node", default="")
    return p.parse_args(argv)


def _exit_on_signal(signum, frame):
    raise SystemExit(128 + signum)  # unwinds into Server.serve()'s cleanup


if __name__ == "__main__":
    if sys.argv[1:] == ["--repair-audio"]:
        repair_audio()
        sys.exit(0)
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, _exit_on_signal)
    Server(parse_args()).serve()
