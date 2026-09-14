#!/usr/bin/env python3
"""
Espone il PC come mouse Bluetooth HID (BR/EDR) e si connette all'Oculus Go.

Prerequisiti (gia' fatti):
  sudo systemctl stop bluetooth
  sudo /usr/lib/bluetooth/bluetoothd --compat --noplugin=input &
  sudo chmod 777 /var/run/sdp
  sudo hciconfig hci0 up piscan
  sudo hciconfig hci0 class 0x002580

Uso:
  sudo python3 btmouse.py

Comandi interattivi una volta connesso:
  w/a/s/d = muovi    c = click sinistro    r = click destro
  W/A/S/D = movimento ampio                q = esci
"""

import os
import sys
import socket
import time

OCULUS_MAC = "2C:26:17:10:AC:0A"

P_CTRL = 17    # PSM control
P_INTR = 19    # PSM interrupt

# Descrittore HID: mouse a 3 pulsanti con movimento relativo X/Y e rotella.
# Report da 4 byte: [bottoni, dx, dy, wheel]
HID_DESCRIPTOR = bytes([
    0x05, 0x01,        # Usage Page (Generic Desktop)
    0x09, 0x02,        # Usage (Mouse)
    0xA1, 0x01,        # Collection (Application)
    0x09, 0x01,        #   Usage (Pointer)
    0xA1, 0x00,        #   Collection (Physical)
    0x05, 0x09,        #     Usage Page (Buttons)
    0x19, 0x01,        #     Usage Minimum (1)
    0x29, 0x03,        #     Usage Maximum (3)
    0x15, 0x00,        #     Logical Minimum (0)
    0x25, 0x01,        #     Logical Maximum (1)
    0x95, 0x03,        #     Report Count (3)
    0x75, 0x01,        #     Report Size (1)
    0x81, 0x02,        #     Input (Data, Variable, Absolute)
    0x95, 0x01,        #     Report Count (1)
    0x75, 0x05,        #     Report Size (5)
    0x81, 0x01,        #     Input (Constant) -- padding
    0x05, 0x01,        #     Usage Page (Generic Desktop)
    0x09, 0x30,        #     Usage (X)
    0x09, 0x31,        #     Usage (Y)
    0x09, 0x38,        #     Usage (Wheel)
    0x15, 0x81,        #     Logical Minimum (-127)
    0x25, 0x7F,        #     Logical Maximum (127)
    0x75, 0x08,        #     Report Size (8)
    0x95, 0x03,        #     Report Count (3)
    0x81, 0x06,        #     Input (Data, Variable, Relative)
    0xC0,              #   End Collection
    0xC0,              # End Collection
])


def hid_descriptor_hex():
    return "".join("%02X" % b for b in HID_DESCRIPTOR)


SDP_RECORD_XML = """<?xml version="1.0" encoding="UTF-8" ?>
<record>
  <attribute id="0x0001">
    <sequence><uuid value="0x1124" /></sequence>
  </attribute>
  <attribute id="0x0004">
    <sequence>
      <sequence>
        <uuid value="0x0100" />
        <uint16 value="0x0011" />
      </sequence>
      <sequence><uuid value="0x0011" /></sequence>
    </sequence>
  </attribute>
  <attribute id="0x0005">
    <sequence><uuid value="0x1002" /></sequence>
  </attribute>
  <attribute id="0x0006">
    <sequence>
      <uint16 value="0x656e" />
      <uint16 value="0x006a" />
      <uint16 value="0x0100" />
    </sequence>
  </attribute>
  <attribute id="0x0009">
    <sequence>
      <sequence>
        <uuid value="0x1124" />
        <uint16 value="0x0100" />
      </sequence>
    </sequence>
  </attribute>
  <attribute id="0x000d">
    <sequence>
      <sequence>
        <sequence>
          <uuid value="0x0100" />
          <uint16 value="0x0013" />
        </sequence>
        <sequence><uuid value="0x0011" /></sequence>
      </sequence>
    </sequence>
  </attribute>
  <attribute id="0x0100"><text value="BT Mouse" /></attribute>
  <attribute id="0x0101"><text value="Bluetooth HID Mouse" /></attribute>
  <attribute id="0x0102"><text value="Linux" /></attribute>
  <attribute id="0x0200"><uint16 value="0x0100" /></attribute>
  <attribute id="0x0201"><uint16 value="0x0111" /></attribute>
  <attribute id="0x0202"><uint8 value="0x40" /></attribute>
  <attribute id="0x0203"><uint8 value="0x00" /></attribute>
  <attribute id="0x0204"><boolean value="false" /></attribute>
  <attribute id="0x0205"><boolean value="true" /></attribute>
  <attribute id="0x0206">
    <sequence>
      <sequence>
        <uint8 value="0x22" />
        <text encoding="hex" value="{desc}" />
      </sequence>
    </sequence>
  </attribute>
  <attribute id="0x0207">
    <sequence>
      <sequence>
        <uint16 value="0x0409" />
        <uint16 value="0x0100" />
      </sequence>
    </sequence>
  </attribute>
  <attribute id="0x020b"><uint16 value="0x0100" /></attribute>
  <attribute id="0x020c"><uint16 value="0x0c80" /></attribute>
  <attribute id="0x020d"><boolean value="true" /></attribute>
  <attribute id="0x020e"><boolean value="false" /></attribute>
</record>
""".replace("{desc}", hid_descriptor_hex())


def register_sdp():
    """Registra il record SDP HID tramite dbus (profilo BlueZ)."""
    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop

    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    path = "/tmp/btmouse_sdp.xml"
    with open(path, "w") as f:
        f.write(SDP_RECORD_XML)

    manager = dbus.Interface(
        bus.get_object("org.bluez", "/org/bluez"),
        "org.bluez.ProfileManager1",
    )

    opts = {
        "ServiceRecord": SDP_RECORD_XML,
        "Role": "server",
        "RequireAuthentication": dbus.Boolean(False),
        "RequireAuthorization": dbus.Boolean(False),
        "AutoConnect": dbus.Boolean(True),
    }

    try:
        manager.RegisterProfile("/org/bluez/btmouse", "00001124-0000-1000-8000-00805f9b34fb", opts)
        print("[+] Profilo HID registrato su D-Bus")
        return True
    except Exception as e:
        print("[!] RegisterProfile fallito: %s" % e)
        print("[ ] Provo comunque: il record potrebbe essere gia' presente.")
        return False


class BTMouse:
    def __init__(self, target):
        self.target = target
        self.ctrl = None
        self.intr = None

    def connect_out(self):
        """Il PC inizia la connessione verso il visore."""
        print("[ ] Connessione verso %s ..." % self.target)
        self.ctrl = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP)
        self.intr = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP)
        self.ctrl.connect((self.target, P_CTRL))
        print("[+] Control channel (PSM 17) aperto")
        self.intr.connect((self.target, P_INTR))
        print("[+] Interrupt channel (PSM 19) aperto")

    def listen_in(self, timeout=60):
        """Il PC si mette in ascolto e aspetta che sia il visore a connettersi."""
        s_ctrl = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP)
        s_intr = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP)
        s_ctrl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s_intr.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s_ctrl.bind(("00:00:00:00:00:00", P_CTRL))
        s_intr.bind(("00:00:00:00:00:00", P_INTR))
        s_ctrl.listen(1)
        s_intr.listen(1)
        print("[ ] In ascolto su PSM 17 e 19. Ora accoppia/connetti dal visore...")
        s_ctrl.settimeout(timeout)
        s_intr.settimeout(timeout)
        self.ctrl, cinfo = s_ctrl.accept()
        print("[+] Control channel da %s" % (cinfo,))
        self.intr, iinfo = s_intr.accept()
        print("[+] Interrupt channel da %s" % (iinfo,))

    def send(self, buttons=0, dx=0, dy=0, wheel=0):
        dx = max(-127, min(127, int(dx))) & 0xFF
        dy = max(-127, min(127, int(dy))) & 0xFF
        wheel = max(-127, min(127, int(wheel))) & 0xFF
        # 0xA1 = DATA | Input report
        report = bytes([0xA1, 0x01, buttons & 0x07, dx, dy, wheel])
        self.intr.send(report)

    def move(self, dx, dy):
        self.send(0, dx, dy, 0)

    def click(self, button=1):
        self.send(button, 0, 0, 0)
        time.sleep(0.05)
        self.send(0, 0, 0, 0)

    def close(self):
        for s in (self.intr, self.ctrl):
            if s:
                try:
                    s.close()
                except Exception:
                    pass


def repl(mouse):
    print()
    print("  w/a/s/d = muovi (10px)    W/A/S/D = muovi (50px)")
    print("  c = click sx    r = click dx    q = esci")
    print()
    while True:
        try:
            cmd = input("mouse> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not cmd:
            continue
        for ch in cmd:
            step = 50 if ch.isupper() else 10
            c = ch.lower()
            try:
                if c == "w":
                    mouse.move(0, -step)
                elif c == "s":
                    mouse.move(0, step)
                elif c == "a":
                    mouse.move(-step, 0)
                elif c == "d":
                    mouse.move(step, 0)
                elif c == "c":
                    mouse.click(1)
                elif c == "r":
                    mouse.click(2)
                elif c == "q":
                    return
                else:
                    continue
                time.sleep(0.02)
            except Exception as e:
                print("[!] Errore invio: %s" % e)
                return


def main():
    if os.geteuid() != 0:
        print("Serve root: sudo python3 btmouse.py")
        sys.exit(1)

    target = sys.argv[1] if len(sys.argv) > 1 else OCULUS_MAC
    mode = sys.argv[2] if len(sys.argv) > 2 else "out"

    register_sdp()
    time.sleep(1)

    mouse = BTMouse(target)
    try:
        if mode == "in":
            mouse.listen_in()
        else:
            mouse.connect_out()
    except Exception as e:
        print("[!] Connessione fallita: %s" % e)
        print()
        print("Se il modo 'out' fallisce, prova il modo 'in':")
        print("  sudo python3 btmouse.py %s in" % target)
        print("e poi dal visore, in Funzioni sperimentali, connetti 'kaybeecorp'.")
        mouse.close()
        sys.exit(1)

    print("[+] Connesso. Invio report HID.")
    try:
        repl(mouse)
    finally:
        mouse.close()
        print("[ ] Chiuso.")


if __name__ == "__main__":
    main()
