// mpe-clean: sits between a raw MPE controller (Seaboard BLOCK) and anything
// that wants MPE (synths, DAWs, other programs), and makes sure the stream is safe.
//
//   Seaboard BLOCK  ->  [MPE-Clean:in]  ... checks ...  [MPE-Clean:out]  ->  synth / DAW
//
// What "clean" means:
//   * every note-on is eventually followed by a note-off, even if the device
//     is unplugged, the MIDI buffer overflows, or an all-notes-off arrives
//   * a note-on on a channel that still holds a note first ends the old note
//   * a note-off for a note that is not held is dropped
//   * the MPE configuration message (lower zone, 15 member channels) is sent at
//     start, when the device (re)appears, and when something connects to :out
//
// Not a synth: it makes no sound. Everything else the device sends is passed
// through unchanged.

#include <alsa/asoundlib.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

static const int kChannels = 16;
static std::atomic<bool> gRun{true};

// Messages the connect thread sends to the main thread (as user events), so
// that only the main thread ever writes to the output port.
enum { kMsgPanic = 0, kMsgSendMpeConfig = 1 };

struct Bridge {
    snd_seq_t* seq = nullptr;
    int client = 0, inPort = 0, outPort = 0;
    int held[kChannels];                    // note held on each channel, -1 = none

    Bridge() { for (int& h : held) h = -1; }

    void send(snd_seq_event_t ev) {
        snd_seq_ev_set_source(&ev, outPort);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq, &ev);
    }
    void noteOff(int ch, int note, int vel = 0) {
        snd_seq_event_t e; snd_seq_ev_clear(&e);
        snd_seq_ev_set_noteoff(&e, ch, note, vel);
        send(e);
    }
    void cc(int ch, int param, int value) {
        snd_seq_event_t e; snd_seq_ev_clear(&e);
        snd_seq_ev_set_controller(&e, ch, param, value);
        send(e);
    }

    // End every note we have started. Also centre bend and zero pressure so the
    // next note on that channel does not start from a leftover value.
    void panic(const char* why) {
        int n = 0;
        for (int ch = 0; ch < kChannels; ch++) {
            if (held[ch] >= 0) { noteOff(ch, held[ch]); held[ch] = -1; n++; }
            snd_seq_event_t e;
            snd_seq_ev_clear(&e); snd_seq_ev_set_pitchbend(&e, ch, 0); send(e);
            snd_seq_ev_clear(&e); snd_seq_ev_set_chanpress(&e, ch, 0); send(e);
            cc(ch, 123, 0);                 // all notes off
        }
        printf("panic (%s): released %d held note%s\n", why, n, n == 1 ? "" : "s");
        fflush(stdout);
    }

    // MPE Configuration Message: RPN 6 on the master channel, value = number of
    // member channels. 15 members = the lower zone uses channels 2..16.
    void sendMpeConfig() {
        cc(0, 101, 0); cc(0, 100, 6); cc(0, 6, 15);
        cc(0, 101, 127); cc(0, 100, 127);   // RPN null: stop later CC6 being misread
        printf("sent MPE configuration (lower zone, 15 members)\n");
        fflush(stdout);
    }

    void handle(const snd_seq_event_t& in) {
        if (snd_seq_ev_is_variable_type(&in)) return;       // skip sysex
        int ch = in.data.note.channel & 15;
        switch (in.type) {
        case SND_SEQ_EVENT_NOTEON:
            if (in.data.note.velocity == 0) { endNote(ch, 0); return; }
            if (held[ch] >= 0) noteOff(ch, held[ch]);        // old note never ended
            held[ch] = in.data.note.note;
            send(in);
            return;
        case SND_SEQ_EVENT_NOTEOFF:
            endNote(ch, in.data.note.off_velocity ? in.data.note.off_velocity : in.data.note.velocity);
            return;
        case SND_SEQ_EVENT_CONTROLLER:
            if (in.data.control.param == 120 || in.data.control.param == 123) { panic("all notes off from device"); return; }
            send(in);
            return;
        case SND_SEQ_EVENT_PITCHBEND:
        case SND_SEQ_EVENT_CHANPRESS:
        case SND_SEQ_EVENT_KEYPRESS:
        case SND_SEQ_EVENT_PGMCHANGE:
            send(in);
            return;
        case SND_SEQ_EVENT_PORT_SUBSCRIBED:
            // someone connected to our output: they missed the MPE config
            if (in.data.connect.sender.client == client && in.data.connect.sender.port == outPort) sendMpeConfig();
            return;
        case SND_SEQ_EVENT_USR0:
            if (in.data.raw32.d[0] == kMsgPanic) panic("device disappeared");
            else if (in.data.raw32.d[0] == kMsgSendMpeConfig) sendMpeConfig();
            return;
        default:
            return;                                          // clocks, announce noise, etc.
        }
    }

    void endNote(int ch, int vel) {
        if (held[ch] < 0) return;                            // nothing held: drop the stray off
        noteOff(ch, held[ch], vel);
        held[ch] = -1;
    }
};

static bool clientPresent(snd_seq_t* seq, const std::string& match) {
    snd_seq_client_info_t* ci;
    snd_seq_client_info_alloca(&ci);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0)
        if (std::string(snd_seq_client_info_get_name(ci)).find(match) != std::string::npos) return true;
    return false;
}

// Subscribe the device's first readable port to our input. Returns true only on a
// NEW connection, so the caller can tell "just appeared" from "still connected".
static bool connectDevice(snd_seq_t* seq, int myClient, int myPort, const std::string& match) {
    snd_seq_client_info_t* ci;
    snd_seq_client_info_alloca(&ci);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        if (std::string(snd_seq_client_info_get_name(ci)).find(match) == std::string::npos) continue;
        int client = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_t* pi;
        snd_seq_port_info_alloca(&pi);
        snd_seq_port_info_set_client(pi, client);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pi);
            if (!(caps & SND_SEQ_PORT_CAP_READ) || !(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;
            snd_seq_port_subscribe_t* sub;
            snd_seq_port_subscribe_alloca(&sub);
            snd_seq_addr_t src = { (unsigned char)client, (unsigned char)snd_seq_port_info_get_port(pi) };
            snd_seq_addr_t dst = { (unsigned char)myClient, (unsigned char)myPort };
            snd_seq_port_subscribe_set_sender(sub, &src);
            snd_seq_port_subscribe_set_dest(sub, &dst);
            if (snd_seq_get_port_subscription(seq, sub) == 0) return false;     // already connected
            if (snd_seq_subscribe_port(seq, sub) == 0) {
                printf("connected to '%s' (%d:%d)\n", snd_seq_client_info_get_name(ci), src.client, src.port);
                fflush(stdout);
                return true;
            }
        }
    }
    return false;
}

static void sendMsg(snd_seq_t* s2, int s2Port, int myClient, int myPort, int msg) {
    snd_seq_event_t e;
    snd_seq_ev_clear(&e);
    e.type = SND_SEQ_EVENT_USR0;
    e.data.raw32.d[0] = msg;
    snd_seq_ev_set_source(&e, s2Port);
    snd_seq_ev_set_dest(&e, myClient, myPort);
    snd_seq_ev_set_direct(&e);
    snd_seq_event_output_direct(s2, &e);
}

int main(int argc, char** argv) {
    std::string match = "Seaboard";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-m" && i + 1 < argc) match = argv[++i];
        else { fprintf(stderr, "usage: %s [-m device-name-substring]\n", argv[0]); return 1; }
    }
    signal(SIGINT, [](int) { gRun = false; });
    signal(SIGTERM, [](int) { gRun = false; });

    Bridge b;
    if (snd_seq_open(&b.seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) { fprintf(stderr, "cannot open ALSA sequencer\n"); return 1; }
    snd_seq_set_client_name(b.seq, "MPE-Clean");
    snd_seq_set_client_pool_input(b.seq, 4000);         // large, so bursts do not overflow
    b.client = snd_seq_client_id(b.seq);
    const unsigned type = SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION;
    b.inPort = snd_seq_create_simple_port(b.seq, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, type);
    b.outPort = snd_seq_create_simple_port(b.seq, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, type);

    // System:Announce tells us when something subscribes to our output.
    snd_seq_connect_from(b.seq, b.inPort, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);

    printf("MPE-Clean ready: in = %d:%d, out = %d:%d, looking for '%s'\n",
           b.client, b.inPort, b.client, b.outPort, match.c_str());
    fflush(stdout);

    // Finds the device, reconnects after a replug, and tells the main thread
    // when the device goes away.
    std::thread watcher([&]() {
        snd_seq_t* s2;
        if (snd_seq_open(&s2, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return;
        snd_seq_set_client_name(s2, "MPE-Clean-watch");
        int s2Port = snd_seq_create_simple_port(s2, "ctl", 0, SND_SEQ_PORT_TYPE_APPLICATION);
        bool wasPresent = false;
        while (gRun) {
            bool present = clientPresent(s2, match);
            if (wasPresent && !present) sendMsg(s2, s2Port, b.client, b.inPort, kMsgPanic);
            if (present && connectDevice(s2, b.client, b.inPort, match))
                sendMsg(s2, s2Port, b.client, b.inPort, kMsgSendMpeConfig);
            wasPresent = present;
            for (int i = 0; i < 5 && gRun; i++) usleep(100000);
        }
        snd_seq_close(s2);
    });

    b.sendMpeConfig();

    // snd_seq_event_input blocks, so poll with a timeout to notice Ctrl-C.
    snd_seq_nonblock(b.seq, 1);
    int nfds = snd_seq_poll_descriptors_count(b.seq, POLLIN);
    struct pollfd* fds = (struct pollfd*)alloca(nfds * sizeof(*fds));
    snd_seq_poll_descriptors(b.seq, fds, nfds, POLLIN);
    while (gRun) {
        if (poll(fds, nfds, 200) <= 0) continue;
        for (;;) {
            snd_seq_event_t* ev = nullptr;
            int r = snd_seq_event_input(b.seq, &ev);
            if (r == -ENOSPC) { b.panic("MIDI buffer overflow"); continue; }
            if (r < 0 || !ev) break;
            b.handle(*ev);
        }
    }
    b.panic("shutting down");
    watcher.join();
    snd_seq_close(b.seq);
    return 0;
}
