# seaboard-mpe

Makes a Seaboard BLOCK (or any MPE controller) safe and easy to use on Linux
without the ROLI app.

```
Seaboard BLOCK -> [MPE-Clean:in] -> checks -> [MPE-Clean:out] -> synth / DAW / bowed
```

`mpe-clean` is a small ALSA MIDI program. It makes no sound. It keeps the MPE
stream well-behaved so nothing downstream gets a stuck note.

## Run

    make
    ./mpe-clean            # looks for a device whose name contains "Seaboard"
    ./mpe-clean -m Roli    # or another name

Then connect `MPE-Clean:out` to your synth (`aconnect -l` lists ports, or use
your DAW's MIDI input list). It reconnects by itself if the device is replugged.

## What "clean" means

- Every note-on gets a note-off, even if the device is unplugged, the MIDI
  buffer overflows, or an all-notes-off arrives.
- A note-on on a channel that still holds a note first ends the old note.
- A note-off for a note that is not held is dropped.
- The MPE configuration message (lower zone, 15 member channels) is sent at
  start, when the device appears, and whenever something connects to `:out`.
  Synths that listen for it switch to MPE mode on their own.
- Everything else (bend, pressure, CC74 slide, ...) passes through unchanged.

## Test

    python3 test_bridge.py

Uses a fake controller, so no hardware is needed. Needs `python3-rtmidi`.

## Not covered

- It does not convert MPE for non-MPE synths.
- Pitch-bend range: the Block sends +/-48 semitones by default. Set the same
  range in the synth. The MPE message sets this for synths that honour it.
- Only the first readable port of the device is used.
