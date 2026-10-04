#!/usr/bin/env python3
"""Write the ALVR v20.14.1 session.json for the Oculus Go client (com.govr.client).

Starts from ALVR's own defaults (session-default-v20.14.1.json, dumped from the pinned source)
and changes only what the Go needs. Usage: make_session.py [output-path]
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
s = json.load(open(os.path.join(HERE, "session-default-v20.14.1.json")))
st = s["session_settings"]
v = st["video"]
v["preferred_codec"]["variant"] = "Hevc"                    # Go: H.264/HEVC only, no AV1
v["foveated_encoding"]["enabled"] = False                    # client presents frames as-is
v["bitrate"]["mode"]["variant"] = "ConstantMbps"
v["bitrate"]["mode"]["ConstantMbps"] = 40                    # USB link measured at 175 Mbit/s
for key in ("transcoding_view_resolution", "emulated_headset_view_resolution"):
    v[key]["variant"] = "Absolute"
    v[key]["Absolute"] = {"width": 1152, "height": {"set": True, "content": 1296}}
st["audio"]["game_audio"]["enabled"] = False                 # audio goes through the govr sink (D-012)
st["audio"]["microphone"]["enabled"] = False
h = st["headset"]
# The Go has no controller in SteamVR mode, but a virtual right controller is sent while the
# SteamVR dashboard is open, driven by the Xbox pad (pc/vr_pointer.py). It is disconnected the
# rest of the time, so games only see the gamepad.
h["controllers"]["enabled"] = True
h["position_recentering_mode"]["variant"] = "Local"          # 3DoF, seated eye height
h["position_recentering_mode"]["Local"] = {"view_height": 1.2}
c = st["connection"]
c["stream_protocol"]["variant"] = "Tcp"                      # adb forward carries TCP only
c["wired_client_type"]["variant"] = "Custom"
c["wired_client_type"]["Custom"] = "com.govr.client"
c["wired_client_autolaunch"] = True
c["client_discovery"]["enabled"] = False                     # wired only
e = st["extra"]
# go-steamvr starts SteamVR itself through vrmonitor.sh: launched by Steam it runs inside the
# Steam Runtime container, where vrserver cannot see vrcompositor and quits after 10 s.
e["steamvr_launcher"]["open_close_steamvr_with_dashboard"] = False
e["open_setup_wizard"] = False
e["new_version_popup"]["enabled"] = False
s["client_connections"] = {
    "client.wired": {"display_name": "Oculus Go (USB)", "current_ip": None, "manual_ips": [],
                     "trusted": True, "connection_state": "Disconnected"}
}
out = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/.config/alvr/session.json")
os.makedirs(os.path.dirname(out), exist_ok=True)
json.dump(s, open(out, "w"), indent=2)
print("wrote", out)
