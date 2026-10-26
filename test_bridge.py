"""End-to-end test for mpe-clean using a fake controller. No hardware needed.

Starts mpe-clean pointed at a fake device ("TestSource"), plays scripted MIDI at
it, and checks what comes out of its output port.
"""
import subprocess, sys, time, os
import rtmidi

HERE = os.path.dirname(os.path.abspath(__file__))
got = []                      # (time, bytes) from the bridge's output port

def on_msg(event, _):
    got.append((time.time(), tuple(event[0])))

def wait(s): time.sleep(s)

def since(mark):
    return [m for t, m in got if t >= mark]

FAKE = """
import sys, rtmidi
m = rtmidi.MidiOut(name="TestSource"); m.open_virtual_port("out")
print("ready", flush=True)
for line in sys.stdin:
    m.send_message([int(x, 16) for x in line.split()])
"""

class Device:
    """A fake controller in its own process. kill() is a real unplug."""
    def __init__(self):
        self.p = subprocess.Popen([sys.executable, "-c", FAKE], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)
        self.p.stdout.readline()
    def send_message(self, msg):
        self.p.stdin.write(" ".join("%02x" % b for b in msg) + "\n")
        self.p.stdin.flush()
    def unplug(self):
        self.p.kill(); self.p.wait()

failures = []
def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + ("" if cond else f"   {detail}"))
    if not cond: failures.append(name)

NOTE_ON, NOTE_OFF, CC, BEND, PRESS = 0x90, 0x80, 0xB0, 0xE0, 0xD0

bridge = subprocess.Popen([os.path.join(HERE, "mpe-clean"), "-m", "TestSource"],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
wait(0.7)

probe = rtmidi.MidiIn(name="Probe")
probe.set_callback(on_msg)
ports = probe.get_ports()
idx = [i for i, p in enumerate(ports) if "MPE-Clean" in p and "out" in p]
assert idx, f"bridge output port not found in {ports}"

# T1: connecting to the output triggers the MPE configuration message
mark = time.time()
probe.open_port(idx[0])
wait(0.5)
msgs = since(mark)
check("MPE config sent when a listener connects",
      (0xB0, 101, 0) in msgs and (0xB0, 100, 6) in msgs and (0xB0, 6, 15) in msgs, msgs)

# fake controller appears
src = Device()
wait(1.5)                                    # watcher polls every 0.5 s

# T2: normal playing passes through unchanged
mark = time.time()
src.send_message([NOTE_ON | 1, 60, 80])
src.send_message([BEND | 1, 0x00, 0x50])
src.send_message([PRESS | 1, 90])
src.send_message([CC | 1, 74, 64])
wait(0.2)
src.send_message([NOTE_OFF | 1, 60, 0])
wait(0.3)
m = since(mark)
check("note on passes through", (NOTE_ON | 1, 60, 80) in m, m)
check("pitch bend passes through", (BEND | 1, 0x00, 0x50) in m, m)
check("pressure passes through", (PRESS | 1, 90) in m, m)
check("slide (CC74) passes through", (CC | 1, 74, 64) in m, m)
check("note off passes through", any(x[0] == NOTE_OFF | 1 and x[1] == 60 for x in m), m)

# T3: a second note-on on a busy channel first ends the old note
mark = time.time()
src.send_message([NOTE_ON | 2, 60, 70])
src.send_message([NOTE_ON | 2, 62, 70])
wait(0.3)
m = since(mark)
off_i = next((i for i, x in enumerate(m) if x[0] == NOTE_OFF | 2 and x[1] == 60), None)
on_i = next((i for i, x in enumerate(m) if x == (NOTE_ON | 2, 62, 70)), None)
check("double note-on: old note ended first", off_i is not None and on_i is not None and off_i < on_i, m)
src.send_message([NOTE_OFF | 2, 62, 0])
wait(0.2)

# T4: a note-off for a note that is not held is dropped
mark = time.time()
src.send_message([NOTE_OFF | 3, 55, 0])
wait(0.3)
check("stray note-off dropped", not any(x[0] == NOTE_OFF | 3 for x in since(mark)), since(mark))

# T5: two notes held, the device disappears -> both must be ended
src.send_message([NOTE_ON | 4, 48, 80])
src.send_message([NOTE_ON | 5, 52, 80])
wait(0.3)
mark = time.time()
src.unplug()                                 # client vanishes, like an unplug
wait(1.5)
m = since(mark)
check("unplug: held note on ch5 ended", any(x[0] == NOTE_OFF | 4 and x[1] == 48 for x in m), m)
check("unplug: held note on ch6 ended", any(x[0] == NOTE_OFF | 5 and x[1] == 52 for x in m), m)
check("unplug: all-notes-off sent", any(x[0] & 0xF0 == CC and x[1] == 123 for x in m), m)

# T6: device returns -> reconnects and sends the MPE config again
mark = time.time()
src = Device()
wait(1.5)
m = since(mark)
check("replug: MPE config sent again", (0xB0, 6, 15) in m, m)
mark = time.time()
src.send_message([NOTE_ON | 1, 64, 80])
wait(0.3)
check("replug: notes flow again", (NOTE_ON | 1, 64, 80) in since(mark), since(mark))

# T7: all-notes-off from the device ends held notes
mark = time.time()
src.send_message([CC | 0, 123, 0])
wait(0.3)
m = since(mark)
check("device all-notes-off ends held note", any(x[0] == NOTE_OFF | 1 and x[1] == 64 for x in m), m)

src.unplug()
probe.close_port()
bridge.terminate()
out, _ = bridge.communicate(timeout=3)
print("\n--- bridge log ---\n" + out.strip())
print(f"\n{len(failures)} failed" if failures else "\nall passed")
sys.exit(1 if failures else 0)
