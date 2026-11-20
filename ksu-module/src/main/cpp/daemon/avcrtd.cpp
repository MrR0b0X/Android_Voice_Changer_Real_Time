// avcrtd — AVCRT cellular voice-changer service (clean replace).
//
// A persistent root daemon. It watches the engine on/off + preset (via ConfigStore,
// the same active_preset.json + avcrt.config.revision the app already writes) and the
// live cellular-call state, and when both are active it engages a CLEAN voice replace:
// the far end hears ONLY the processed (pitched) voice; the real voice is removed.
//
// Mechanism (fully adaptive — no hardcoded session/decimator/port):
//   * The kernel "AVCRT Dec Mute" kcontrol (tx-macro.c patch) mutes, at the codec, the
//     decimators that feed the call's modem uplink -> real voice removed.
//   * A spare FREE decimator (unmuted) taps the same mic -> a free capture port -> the
//     daemon reads the LOUD real voice, pitches it (avcrt::Engine), and injects it via
//     incall_music into the call's CVS -> far end hears only the processed voice.
//
// Self-managing: engages when (engine on && call up), disengages + fully restores when
// the call ends or the engine is turned off. Run as root, persistently.

#include <tinyalsa/asoundlib.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "avcrt_engine.h"
#include "avcrt_config.h"

using avcrt::ConfigStore;
using avcrt::DspConfig;
using avcrt::Engine;

namespace {

// Supported Qualcomm cards: litomtpsndcard (lito-mtp-snd-card) and
// lahainayupikidp (lahaina-yupikidp-snd-card). Card numbering can shift when USB audio registers,
// so resolve by card ID/description rather than assuming ALSA card 0.
unsigned       g_card      = 0;
int            g_usb_card  = -1;       // USB headset sound card index (can be 0!); -1 = none
constexpr unsigned kCapDevice = 0;     // MultiMedia1 (pcm <card>,0) capture FE
constexpr unsigned kInjDevice = 23;    // MultiMedia9 (pcm <card>,23) playback FE
constexpr unsigned kUsbCapDev = 0;     // USB card capture pcm (card<usb>,0)
constexpr unsigned kRate      = 16000;
constexpr unsigned kChannels  = 1;
constexpr unsigned kPeriod    = 256;
constexpr unsigned kPeriods   = 4;
constexpr unsigned kUsbRate   = 48000; // USB headset mic native rate (S16 mono); decimated 3:1 -> 16k
constexpr unsigned kUsbDecim  = kUsbRate / kRate;        // 3
constexpr unsigned kUsbPeriod = kPeriod * kUsbDecim;     // 768 USB frames -> 256 @16k
constexpr int      kIdlePollMs = 400;  // how often we look for a call while idle

// Capture/engage mode for the current call.
//  MODE_BT = a Bluetooth (HFP/SCO) headset call — the uplink sources from INT_BT_SCO_TX
//  (the BT chip <-> ADSP SCO bridge), handled like MODE_HEADSET (reroute to the Qualcomm codec +
//  capture the freed mic + incall_music inject), but the BT mic is on the Qualcomm card.
enum Mode { MODE_NONE = 0, MODE_BUILTIN = 1, MODE_HEADSET = 2, MODE_BT = 3 };

// The incall_music inject path is structurally quieter than the real mic leg, so
// we add a fixed compensation on top of the preset's artistic gain to land the
// processed voice at roughly original loudness on the far end. Empirically ~+22 dB.
constexpr double   kInjectGainCompDb = 22.0;

const char* kDecMuteCtl = "AVCRT Dec Mute";
// In-kernel surgical vocproc (CVP/CVS) rebuild for the live call — flushes a stale CVP Tx-device
// latch WITHOUT restarting audioserver (no USB re-enumeration flicker). Replaces the "hammer" for
// every accessory transition when the AVCRT unified kernel patch is flashed. Default-off kcontrol.
const char* kCvpRebuildCtl = "AVCRT CVP Rebuild";

std::atomic<bool> g_run{true};
struct mixer*     g_mixer = nullptr;
bool              g_patch_present = false;   // "AVCRT Dec Mute" kcontrol exists (AVCRT kernel)
bool              g_cvp_rebuild_present = false;   // "AVCRT CVP Rebuild" kcontrol exists (unified patch)

void on_signal(int) { g_run.store(false); }

void sleep_ms(int ms) {
    struct timespec ts { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
}

// Find a supported Qualcomm audio card by ID or description in /proc/asound/cards.
// The Lahaina/Yupik card configuration in this kernel tree uses Bolero TX macros.
// Returns card 0 as a best-effort fallback for legacy devices if probing fails.
unsigned find_audio_card() {
    FILE* f = fopen("/proc/asound/cards", "re");
    if (!f) return 0;
    char line[256];
    int idx = -1;
    unsigned found = 0;
    bool got = false;
    while (fgets(line, sizeof(line), f)) {
        int n;
        if (sscanf(line, " %d [", &n) == 1) idx = n;   // a new card header line
        if (idx >= 0 && (strstr(line, "litomtp") || strstr(line, "lito-mtp-snd") ||
                         strstr(line, "lahainayupikidp") ||
                         strstr(line, "lahaina-yupikidp-snd-card"))) {
            found = (unsigned)idx; got = true; break;
        }
    }
    fclose(f);
    return got ? found : 0;
}

// Find a USB-Audio sound card (a USB/USB-C headset or a USB-C->jack DAC adapter) in
// /proc/asound/cards. Returns the index, or -1 if no USB card is present (no headset).
// Re-scanned each idle poll since a headset can be plugged/unplugged at runtime.
int find_usb_card() {
    FILE* f = fopen("/proc/asound/cards", "re");
    if (!f) return -1;
    char line[256];
    int idx = -1, found = -1;
    while (fgets(line, sizeof(line), f)) {
        int n;
        if (sscanf(line, " %d [", &n) == 1) idx = n;
        // USB-Audio cards advertise "USB-Audio" / "USB Audio" on the description line.
        if (idx >= 0 && (strstr(line, "USB-Audio") || strstr(line, "USB Audio"))) {
            found = idx; break;
        }
    }
    fclose(f);
    return found;
}

// (Re)resolve the Qualcomm + USB card indices BY NAME and reopen its mixer if it moved.
// CRITICAL: ALSA card indices RENUMBER across boot enumeration + audioserver restarts — e.g. with a
// type-c headset plugged at BOOT the USB card grabs card 0 and lito becomes card 1; a hammer
// (audioserver restart) can also re-enumerate. g_card/g_mixer used to be cached ONCE at startup, so
// after a renumber the daemon wrote to the WRONG card (the USB Earphone) → voice controls no-op →
// the type-c "changed voice + original from the phone bottom mic" chaos + churn. Call this whenever
// the card set may have changed (startup, idle poll, after a hammer) so each operation hits the
// audio DSP card instead of a USB card.
void resolve_cards() {
    unsigned audio_card = find_audio_card();
    if (!g_mixer || audio_card != g_card) {
        if (g_mixer) { mixer_close(g_mixer); g_mixer = nullptr; }
        g_card  = audio_card;
        g_mixer = mixer_open(g_card);
        fprintf(stderr, "[avcrtd] resolve_cards: audio=card %u (mixer %s)\n",
                g_card, g_mixer ? "ok" : "FAILED");
    }
    g_usb_card = find_usb_card();
    if (g_usb_card == (int)g_card) g_usb_card = -1;   // never treat lito as the USB card (flux guard)
}

void mx_set(const char* name, int value) {
    if (!g_mixer) return;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) { fprintf(stderr, "[avcrtd] ctl not found: %s\n", name); return; }
    int n = mixer_ctl_get_num_values(c);
    for (int i = 0; i < n; ++i) mixer_ctl_set_value(c, i, value);
}

void mx_enum(const char* name, const char* val) {
    if (!g_mixer) return;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return;
    mixer_ctl_set_enum_by_string(c, val);
}

// Returns the first value of a control, or -1 if absent.
int mx_get(const char* name) {
    if (!g_mixer) return -1;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return -1;
    return mixer_ctl_get_value(c, 0);
}

// Current STRING value of an ENUM control (e.g. a mux source). Writes into out (size sz);
// out is "" if the control is absent / not an enum. Used to DISCOVER, not assume, the mic
// source feeding the call's modem decimator (digital DMICx vs analog ADCx via SoundWire).
void mx_get_enum(const char* name, char* out, size_t sz) {
    if (sz) out[0] = '\0';
    if (!g_mixer) return;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return;
    int idx = mixer_ctl_get_value(c, 0);
    const char* s = mixer_ctl_get_enum_string(c, idx);
    if (s) snprintf(out, sz, "%s", s);
}

// Any value of a control non-zero?
bool mx_any_on(const char* name) {
    if (!g_mixer) return false;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return false;
    int n = mixer_ctl_get_num_values(c);
    for (int i = 0; i < n; ++i)
        if (mixer_ctl_get_value(c, i) > 0) return true;
    return false;
}

// ---- adaptive call/session/decimator detection -----------------------------

// Is the device in an ACTIVE call? On WiFi-Calling (VoWiFi) devices the CVD voice
// session (pcm15 / VoiceMMode Tx mixer) stays RUNNING permanently even when idle,
// so the mixer state is NOT a reliable "call up" signal. Audio mode is also no good
// on this device: `mode (internal)` stays NORMAL even during a real call, and
// `Actual mode` can stay stale IN_CALL with no call. The reliable signal is the
// telephony call state: `dumpsys telephony.registry` reports mCallState=2 (OFFHOOK)
// for an active call on a SIM (0=idle, 1=ringing). Cached ~1s (cheap).
bool is_in_call_uncached() {
    // CS (circuit-switched) call: telephony mCallState=2 (OFFHOOK).
    FILE* p = popen("dumpsys telephony.registry 2>/dev/null", "r");
    if (p) {
        char line[512];
        while (fgets(line, sizeof(line), p)) {
            const char* m = strstr(line, "mCallState=");
            if (m && m[11] == '2') { pclose(p); return true; }
        }
        pclose(p);
    }
    // VoLTE/IMS call: mCallState often stays 0; the audio HAL's "Actual mode = MODE_IN_CALL" catches
    // it. A STALE Actual-mode after a call ends is NOT a false-engage risk because detect_mode also
    // requires the voice Tx mixer to be routed (which is Off once the call really ends).
    p = popen("dumpsys audio 2>/dev/null", "r");
    if (p) {
        char line[512];
        while (fgets(line, sizeof(line), p)) {
            if (strstr(line, "Actual mode") && strstr(line, "MODE_IN_CALL")) { pclose(p); return true; }
        }
        pclose(p);
    }
    return false;
}

// Call state is polled by a BACKGROUND thread (dumpsys forks a process and blocks
// ~100ms — doing that in the audio loop between pcm_read/pcm_write underruns the
// stream → choppy audio + engage/disengage flapping). The audio loop only reads
// this atomic, which is instant.
std::atomic<bool> g_in_call{false};

void* call_monitor(void*) {
    while (g_run.load()) {
        g_in_call.store(is_in_call_uncached());
        // poll ~once a second; break the sleep into chunks so we exit promptly
        for (int i = 0; i < 10 && g_run.load(); ++i) sleep_ms(100);
    }
    return nullptr;
}

bool is_in_call() { return g_in_call.load(); }

// Active VoiceMMode session: 1, 2, or 0 if no cellular call is up. Requires BOTH
// the voice Tx mixer route AND a real in-call audio mode (VoWiFi-safe).
int detect_session() {
    if (!is_in_call()) return 0;
    if (mx_any_on("VoiceMMode2_Tx Mixer TX_CDC_DMA_TX_3_MMode2")) return 2;
    if (mx_any_on("VoiceMMode1_Tx Mixer TX_CDC_DMA_TX_3_MMode1")) return 1;
    return 0;
}

// ---- no-hardcode codec mic-path discovery ----------------------------------
// The call's modem-uplink mic is NOT always the built-in DMIC on TX_AIF1/TX_CDC_DMA_TX_3.
// CONFIRMED on device: built-in mic = DMIC -> DECn -> TX_AIF1 -> TX_CDC_DMA_TX_3; an ANALOG
// headset (Type-C->3.5mm adapter) = ADC1 -> SWR_MIC -> DEC5 -> TX_AIF2 -> TX_CDC_DMA_TX_4.
// So we DISCOVER the live path each call (which VoiceMMode_Tx source, which AIF, which
// decimator, which mic) and replicate the SAME source on a spare decimator — no hardcoded
// TX_3 / AIF1 / DMIC1. One code path covers built-in + analog-headset (+ anything else codec).

// A decimator's mic source, so we can replicate it on a spare decimator for capture.
struct MicSrc {
    char dec_mux[16] = {0};   // "MSM_DMIC" (digital) or "SWR_MIC" (analog/SoundWire ADC)
    char sub_mux[16] = {0};   // DMICx (digital) or ADCx / SWR_DMICx (analog)
    bool valid = false;
};

// What mic source is decimator d reading right now?
MicSrc discover_mic_source(int d) {
    MicSrc m;
    char dec[20], dmic[20], smic[20], decv[16];
    snprintf(dec,  sizeof(dec),  "TX DEC%d MUX",  d);
    snprintf(dmic, sizeof(dmic), "TX DMIC MUX%d", d);
    snprintf(smic, sizeof(smic), "TX SMIC MUX%d", d);
    mx_get_enum(dec, decv, sizeof(decv));
    if (strcmp(decv, "SWR_MIC") == 0) {
        snprintf(m.dec_mux, sizeof(m.dec_mux), "SWR_MIC");
        mx_get_enum(smic, m.sub_mux, sizeof(m.sub_mux));     // ADCx / SWR_DMICx
    } else {
        snprintf(m.dec_mux, sizeof(m.dec_mux), "MSM_DMIC");
        mx_get_enum(dmic, m.sub_mux, sizeof(m.sub_mux));     // DMICx
    }
    m.valid = m.sub_mux[0] && strcmp(m.sub_mux, "ZERO") != 0;
    return m;
}

// Bitmask of decimators with a REAL (non-ZERO) mic source routed into TX_AIF<aif>_CAP.
int decs_in_aif(int aif) {
    int mask = 0;
    char name[40];
    for (int i = 0; i < 8; ++i) {
        snprintf(name, sizeof(name), "TX_AIF%d_CAP Mixer DEC%d", aif, i);
        if (mx_get(name) > 0 && discover_mic_source(i).valid) mask |= (1 << i);
    }
    return mask;
}

// Back-compat: the modem mics on the built-in AIF1 path (used by morph_to_builtin).
int detect_modem_mask() { return decs_in_aif(1); }

// Which VoiceMMode<n>_Tx codec backend is the modem reading? Sets session (1/2) + the AIF
// that carries the mic (1 for TX_CDC_DMA_TX_3, 2 for TX_4; others resolved by real-source AIF).
// Returns false if there's no codec uplink (no call, or a USB-card uplink). Scans the codec
// DMA TX channels the bolero TX-macro exposes (0..5) — bounded hardware, not a topology guess.
bool discover_codec_uplink(int& session, int& aif) {
    char name[64];
    for (int n = 1; n <= 2; ++n) {
        for (int tx = 0; tx <= 5; ++tx) {
            snprintf(name, sizeof(name), "VoiceMMode%d_Tx Mixer TX_CDC_DMA_TX_%d_MMode%d", n, tx, n);
            if (mx_any_on(name)) {
                session = n;
                if      (tx == 3) aif = 1;
                else if (tx == 4) aif = 2;
                else              aif = decs_in_aif(1) ? 1 : (decs_in_aif(2) ? 2 : 1);
                return true;
            }
        }
    }
    return false;
}

// A free spare decimator (not a modem dec) for our parallel capture. Prefer HIGH indices
// (DEC3+) so it can't collide with the modem's typically-low decimators or a transition latch.
int pick_spare_dec(int modem_mask) {
    for (int i = 3; i < 8; ++i) if (!(modem_mask & (1 << i))) return i;
    for (int i = 0; i < 3; ++i) if (!(modem_mask & (1 << i))) return i;
    return 7;
}

// Decide which clean-replace flow this call supports + the VoiceMMode session (1/2).
//  - USB-card headset on the uplink (VoiceMMode*_Tx <- USB_AUDIO_TX) -> MODE_HEADSET (reroute flow).
//  - Codec mic on the uplink (built-in DMIC *or* analog-headset ADC, DISCOVERED) -> MODE_BUILTIN.
//    Clean-replaced via AVCRT Dec Mute -> needs the AVCRT kernel patch.
//  - else MODE_NONE (idle).
Mode detect_mode(int& session) {
    session = 0;
    if (!is_in_call()) return MODE_NONE;
    if (mx_any_on("VoiceMMode2_Tx Mixer USB_AUDIO_TX_MMode2")) { session = 2; return MODE_HEADSET; }
    if (mx_any_on("VoiceMMode1_Tx Mixer USB_AUDIO_TX_MMode1")) { session = 1; return MODE_HEADSET; }
    // Bluetooth (HFP/SCO) headset call: the uplink sources from INT_BT_SCO_TX. Detected like USB but
    // a BT device is NOT a USB sound card, so this is checked before the USB-card guard below and the
    // BT flow (engage_bt) frees + captures the BT mic on the Qualcomm audio card.
    if (mx_any_on("VoiceMMode2_Tx Mixer INT_BT_SCO_TX_MMode2")) { session = 2; return MODE_BT; }
    if (mx_any_on("VoiceMMode1_Tx Mixer INT_BT_SCO_TX_MMode1")) { session = 1; return MODE_BT; }
    // A USB-Audio CARD is present but the uplink isn't on USB yet (AHAL mid-transition / leftover
    // reroute) -> stay idle until the USB uplink appears (don't run the codec flow with a USB headset
    // = the double-voice regression). NOTE: an analog Type-C->jack adapter is NOT a USB card, so it
    // correctly falls through to the codec flow below.
    if (find_usb_card() >= 0) return MODE_NONE;
    int aif = 0;
    if (g_patch_present && discover_codec_uplink(session, aif)) return MODE_BUILTIN;
    return MODE_NONE;
}

pcm_config make_cfg() {
    pcm_config c; memset(&c, 0, sizeof(c));
    c.channels = kChannels; c.rate = kRate;
    c.period_size = kPeriod; c.period_count = kPeriods;
    c.format = PCM_FORMAT_S16_LE;
    c.start_threshold = kPeriod; c.stop_threshold = kPeriod * kPeriods;
    return c;
}

// USB headset mic: 48k mono, 768-frame periods (== 256 @16k after 3:1 decimation).
pcm_config make_usb_cfg() {
    pcm_config c; memset(&c, 0, sizeof(c));
    c.channels = 1; c.rate = kUsbRate;
    c.period_size = kUsbPeriod; c.period_count = kPeriods;
    c.format = PCM_FORMAT_S16_LE;
    c.start_threshold = kUsbPeriod; c.stop_threshold = kUsbPeriod * kPeriods;
    return c;
}

// ---- engage / disengage ----------------------------------------------------

struct Session {
    int   mode       = MODE_NONE;
    int   session    = 0;          // VoiceMMode 1 or 2
    int   modem_mask = 0;          // DECs the AHAL shows in the modem leg (discovered AIF)
    int   mute_mask  = 0;          // DECs we actually mute
    int   modem_aif  = 1;          // AIF carrying the modem mic (1=TX_3, 2=TX_4) — DISCOVERED
    int   spare_dec  = 0;
    MicSrc mic;                    // the modem mic source we replicate on the spare (DMIC/ADC)
    char  inj_mixer[48] = {0};     // Incall_Music[_2] Audio Mixer MultiMedia9
    char  cap_from[48]  = {0};     // MultiMedia1 Mixer TX_CDC_DMA_TX_<cap> = our capture FE source
    char  spare_dmic_mux[20] = {0};
    char  spare_smic_mux[20] = {0};
    char  spare_dec_mux[20] = {0};
    char  spare_aif[28] = {0};       // TX_AIF<cap>_CAP Mixer DEC<spare> (our capture leg)
    char  spare_aif1[28] = {0};      // TX_AIF<modem>_CAP Mixer DEC<spare> (the MODEM leg — must stay 0)
    struct pcm* cap = nullptr;     // built-in: spare-DEC capture (pcm <lito>,0)
    struct pcm* inj = nullptr;     // both: incall_music inject (pcm <lito>,23)
    // ---- headset mode only ----
    struct pcm* usb_cap   = nullptr;   // the freed USB headset mic (pcm <usb>,0, 48k mono)
    struct pcm* be_keeper = nullptr;   // keeps the bolero placeholder codec chain powered (pcm <lito>,0)
    char  vtx_usb[44] = {0};           // VoiceMMode<n>_Tx Mixer USB_AUDIO_TX_MMode<n>
    char  vtx_bt[48] = {0};            // VoiceMMode<n>_Tx Mixer INT_BT_SCO_TX_MMode<n> (BT mode)
    char  vtx_bolero[48] = {0};        // VoiceMMode<n>_Tx Mixer TX_CDC_DMA_TX_3_MMode<n>
    bool  engaged = false;
};

const char* kCapFromTx4 = "MultiMedia1 Mixer TX_CDC_DMA_TX_4";
const char* kCapFromTx3 = "MultiMedia1 Mixer TX_CDC_DMA_TX_3";

// Brings up the clean-replace routing + opens the PCMs. Returns false on failure (caller
// disengages). Assumes a codec call is up. NO HARDCODE: DISCOVERS the modem's mic path
// (which AIF, which decimator, which DMIC/ADC source) and replicates the SAME source on a
// spare decimator routed to the OTHER (free) AIF/DMA for capture. Built-in DMIC and an analog
// Type-C->jack headset (ADC via SWR_MIC) are handled by the exact same code.
bool engage(Session& s, int session) {
    s.mode = MODE_BUILTIN;
    s.session = session;

    // DISCOVER the modem's uplink AIF (1=TX_CDC_DMA_TX_3, 2=TX_4) for this session.
    int dsess = session, aif = 1;
    if (!discover_codec_uplink(dsess, aif)) return false;   // no codec uplink (gone)
    s.modem_aif  = aif;
    s.modem_mask = decs_in_aif(aif);                        // the real-source decs feeding the modem
    if (s.modem_mask == 0) return false;                    // not really a call
    // ALSO defensively mute DEC0. After a headset flow (which uses DEC0 as its bolero placeholder)
    // and a swap (type-c→...→codec), the codec DMA / CVP can stay LATCHED feeding the modem's TX
    // port from DEC0 even though TX_AIF only shows the new modem dec → the real voice leaks = the
    // swap ADDITIVE. DEC0 is unused on a fresh codec call → muting it is harmless (proven), and the
    // spare picker avoids the mute set. (This restores the pre-discovery-rewrite belt-and-suspenders
    // that was wrongly dropped — it was for exactly this headset→codec latch, not morph-specific.)
    s.mute_mask  = s.modem_mask | (1 << 0);

    // The modem's PRIMARY decimator + its mic source (DMICx digital / ADCx analog) — to replicate.
    int primary = __builtin_ctz((unsigned)s.modem_mask);
    s.mic = discover_mic_source(primary);
    if (!s.mic.valid) return false;                         // couldn't read the call's mic source

    // Capture via the OTHER AIF/DMA (the modem holds one of the two codec capture pairs; we use
    // the free one: modem on AIF1/TX_3 -> we capture AIF2/TX_4, and vice-versa).
    int cap_aif = (aif == 1) ? 2 : 1;
    const char* cap_dma = (cap_aif == 1) ? kCapFromTx3 : kCapFromTx4;
    const char* off_dma = (cap_aif == 1) ? kCapFromTx4 : kCapFromTx3;
    s.spare_dec = pick_spare_dec(s.mute_mask);   // avoid the modem decs AND the defensively-muted DEC0

    snprintf(s.inj_mixer, sizeof(s.inj_mixer),
             session == 1 ? "Incall_Music Audio Mixer MultiMedia9"
                          : "Incall_Music_2 Audio Mixer MultiMedia9");
    snprintf(s.cap_from,       sizeof(s.cap_from),       "%s", cap_dma);
    snprintf(s.spare_dec_mux,  sizeof(s.spare_dec_mux),  "TX DEC%d MUX",  s.spare_dec);
    snprintf(s.spare_dmic_mux, sizeof(s.spare_dmic_mux), "TX DMIC MUX%d", s.spare_dec);
    snprintf(s.spare_smic_mux, sizeof(s.spare_smic_mux), "TX SMIC MUX%d", s.spare_dec);
    snprintf(s.spare_aif,      sizeof(s.spare_aif),      "TX_AIF%d_CAP Mixer DEC%d", cap_aif, s.spare_dec);
    snprintf(s.spare_aif1,     sizeof(s.spare_aif1),     "TX_AIF%d_CAP Mixer DEC%d", aif,     s.spare_dec);

    fprintf(stderr, "[avcrtd] engage: session=%d modem_aif=%d modem_mask=0x%x mic=%s/%s spare_dec=%d cap_aif=%d inject='%s'\n",
            session, aif, s.modem_mask, s.mic.dec_mux, s.mic.sub_mux, s.spare_dec, cap_aif, s.inj_mixer);

    // 1) spare UNMUTED capture: DEC<spare> reads the SAME mic as the modem (replicated source),
    //    routed to the FREE AIF/DMA -> MM1 (pcm <lito>,0). It must feed ONLY our capture leg, NEVER
    //    the modem leg (else the real voice leaks to the modem AND we pitch it = ADDITIVE).
    mx_set(s.spare_aif1, 0);                       // spare OUT of the modem AIF
    mx_enum(s.spare_dec_mux, s.mic.dec_mux);       // MSM_DMIC or SWR_MIC (same as the modem dec)
    if (strcmp(s.mic.dec_mux, "SWR_MIC") == 0) mx_enum(s.spare_smic_mux, s.mic.sub_mux);  // ADCx
    else                                       mx_enum(s.spare_dmic_mux, s.mic.sub_mux);  // DMICx
    mx_set(s.spare_aif, 1);                        // spare -> our capture AIF
    mx_set(off_dma, 0);
    mx_set(cap_dma, 1);                            // MM1 capture FE <- the free DMA
    // 2) inject path on
    mx_set(s.inj_mixer, 1);

    pcm_config ccfg = make_cfg(), icfg = make_cfg();
    s.cap = pcm_open(g_card, kCapDevice, PCM_IN,  &ccfg);
    s.inj = pcm_open(g_card, kInjDevice, PCM_OUT, &icfg);
    if (!s.cap || !pcm_is_ready(s.cap) || !s.inj || !pcm_is_ready(s.inj)) {
        fprintf(stderr, "[avcrtd] engage pcm_open failed (cap=%s inj=%s)\n",
                s.cap ? pcm_get_error(s.cap) : "null",
                s.inj ? pcm_get_error(s.inj) : "null");
        return false;
    }
    mx_set("Playback 23 Volume", 8192);

    // 3) MUTE the modem decimators -> real voice removed from the uplink.
    mx_set(kDecMuteCtl, s.mute_mask);
    s.engaged = true;
    return true;
}

// Decimate 48k mono -> 16k mono (3:1) by averaging each group of 3 (cheap anti-alias).
// in: kUsbPeriod (768) samples; out: kPeriod (256) samples.
void decimate_3to1(const int16_t* in, int16_t* out, size_t out_frames) {
    for (size_t i = 0; i < out_frames; ++i) {
        int32_t a = in[i * 3] + in[i * 3 + 1] + in[i * 3 + 2];
        out[i] = (int16_t)(a / 3);
    }
}

// ───── HEADSET clean-replace (USB/USB-C-adapter headset, NO kernel patch required) ─────
// Reroute the call uplink USB→bolero (so the modem reads the built-in mic), silence the bolero
// placeholder (DMIC0 silent line; + AVCRT Dec Mute if the patch is present), which FREES the USB
// headset mic; capture the freed headset mic, pitch, and inject via incall_music. The user talks
// into the HEADSET, the far end hears ONLY the pitched headset voice. All stock mechanisms.
bool engage_headset(Session& s, int session) {
    if (g_usb_card < 0) return false;   // no USB headset (g_usb_card resolved by resolve_cards, != lito)
    s.mode = MODE_HEADSET;
    s.session = session;

    snprintf(s.inj_mixer, sizeof(s.inj_mixer),
             session == 1 ? "Incall_Music Audio Mixer MultiMedia9"
                          : "Incall_Music_2 Audio Mixer MultiMedia9");
    snprintf(s.vtx_usb, sizeof(s.vtx_usb),
             "VoiceMMode%d_Tx Mixer USB_AUDIO_TX_MMode%d", session, session);
    snprintf(s.vtx_bolero, sizeof(s.vtx_bolero),
             "VoiceMMode%d_Tx Mixer TX_CDC_DMA_TX_3_MMode%d", session, session);

    fprintf(stderr, "[avcrtd] engage_headset: session=%d usb_card=%d inject='%s'\n",
            session, g_usb_card, s.inj_mixer);

    // 1) bolero PLACEHOLDER mic: DEC0 ← DMIC0 (silent line) → TX_AIF1 → TX_CDC_DMA_TX_3.
    mx_enum("TX DMIC MUX0", "DMIC0");          // clocked-but-silent mic line (patch-free silence)
    mx_enum("TX DEC0 MUX", "MSM_DMIC");
    mx_set("TX_AIF1_CAP Mixer DEC0", 1);
    mx_set("MultiMedia1 Mixer TX_CDC_DMA_TX_3", 1);

    // 2) BE-keeper: open a capture on TX_CDC_DMA_TX_3 to power the codec chain AND make the BE
    //    'active' so the voice-Tx reroute fires voc_set_device_config. Read one period to start it.
    pcm_config bcfg = make_cfg();
    s.be_keeper = pcm_open(g_card, kCapDevice, PCM_IN, &bcfg);
    if (!s.be_keeper || !pcm_is_ready(s.be_keeper)) {
        fprintf(stderr, "[avcrtd] engage_headset: BE-keeper open failed: %s\n",
                s.be_keeper ? pcm_get_error(s.be_keeper) : "null");
        return false;
    }
    {
        int16_t tmp[kPeriod]; pcm_read(s.be_keeper, tmp, sizeof(tmp));   // ensure flowing
    }

    // 3) reroute (ORDER CRITICAL): USB OFF first (voc_disable_device), bolero ON last
    //    (voc_set_device_config(bolero)+voc_enable_device) → modem reads the silent bolero mic.
    mx_set(s.vtx_usb, 0);
    mx_set(s.vtx_bolero, 1);
    // On a patched kernel, hard-mute ALL codec decimators (0xFF). In headset mode the modem must hear
    // ONLY our incall_music inject — the real voice comes from the type-c mic (captured via usb_cap),
    // so NO codec decimator should reach the uplink. Muting only DEC0 (the placeholder) left a leak on
    // a SWAP (analog→type-c): the transient bottom-mic moment latches the CVP/DMA onto DMIC1's dec, and
    // the visible routing (modem←DEC0 muted) looked clean yet the latched dec leaked = additive. Muting
    // all decs (they stay CLOCKED-but-silent) kills any latched dec; harmless on a fresh call (the
    // placeholder DEC0 is muted either way, the rest are unused). type-c capture is unaffected (it's the
    // USB card, not a codec dec).
    if (g_patch_present) mx_set(kDecMuteCtl, 0xFF);

    // 4) the USB headset mic is now FREE — open + PRIME it (card<usb>,0, 48k mono). The reroute
    //    releases the mic from the call's ADSP offload ASYNCHRONOUSLY, so the first start can EPIPE
    //    ("cannot start channel: Broken pipe") when the headset was plugged MID-CALL (the endpoint
    //    isn't ready yet). Retry open+start a few times before giving up — do NOT misread a startup
    //    race as "headset gone" (that fell back to built-in → the phone's bottom mic leaked = additive).
    //    A headset plugged BEFORE the call usually starts on the first try.
    pcm_config ucfg = make_usb_cfg();
    bool usb_started = false;
    for (int attempt = 0; attempt < 12 && g_run.load(); ++attempt) {
        sleep_ms(120);                              // let the ADSP release the USB capture endpoint
        if (find_usb_card() < 0) {                  // headset actually pulled during setup → bail
            fprintf(stderr, "[avcrtd] engage_headset: headset vanished during usb_cap prime\n");
            return false;
        }
        if (!s.usb_cap) s.usb_cap = pcm_open((unsigned)g_usb_card, kUsbCapDev, PCM_IN, &ucfg);
        if (!s.usb_cap || !pcm_is_ready(s.usb_cap)) {
            if (s.usb_cap) { pcm_close(s.usb_cap); s.usb_cap = nullptr; }
            continue;
        }
        int16_t tmp[kUsbPeriod];
        if (pcm_read(s.usb_cap, tmp, sizeof(tmp)) == 0) { usb_started = true; break; }
        // start failed (EPIPE) — close + reopen for a clean retry next attempt
        pcm_close(s.usb_cap); s.usb_cap = nullptr;
    }
    if (!usb_started) {
        fprintf(stderr, "[avcrtd] engage_headset: usb_cap failed to start after retries (headset present) — bail\n");
        return false;
    }
    fprintf(stderr, "[avcrtd] engage_headset: usb_cap started\n");

    // 5) inject path.
    mx_set(s.inj_mixer, 1);
    pcm_config icfg = make_cfg();
    s.inj = pcm_open(g_card, kInjDevice, PCM_OUT, &icfg);
    if (!s.inj || !pcm_is_ready(s.inj)) {
        fprintf(stderr, "[avcrtd] engage_headset: inj open failed: %s\n",
                s.inj ? pcm_get_error(s.inj) : "null");
        return false;
    }
    mx_set("Playback 23 Volume", 8192);

    s.engaged = true;
    return true;
}

// ───── BLUETOOTH (HFP/SCO) clean-replace — mirrors the USB headset flow ─────
// A BT-routed GSM call sources its uplink from INT_BT_SCO_TX (the BT chip <-> ADSP SCO bridge, the
// BT analogue of USB_AUDIO_TX). Same idea as engage_headset: reroute the uplink INT_BT_SCO_TX->bolero
// (the modem reads the silent bolero placeholder), which should FREE the BT SCO mic; capture the freed
// BT mic via MM1 <- INTERNAL_BT_SCO_TX, pitch (avcrt::Engine), inject via incall_music. The BT mic is on
// the Qualcomm audio card (not a separate USB card), so MM1 is reused SEQUENTIALLY: first as the BE-keeper to fire
// the reroute, then re-pointed to the BT mic.
//
// ⚠️ LIVE-VALIDATION PATH — needs a real BT (HFP) GSM call to confirm two unknowns the USB flow did NOT
// have: (1) whether the SCO link SURVIVES the uplink reroute off it (the BT stack owns the HFP link and
// may tear the SCO mic down), and (2) whether INTERNAL_BT_SCO_TX is AP-capturable once the call is off it
// (the same ADSP-bridge concurrency question USB had). engage_bt logs each step and BAILS SAFELY (returns
// false -> the daemon idles for this BT call, the call itself is unaffected) if the capture can't start.
bool engage_bt(Session& s, int session) {
    s.mode = MODE_BT;
    s.session = session;

    snprintf(s.inj_mixer, sizeof(s.inj_mixer),
             session == 1 ? "Incall_Music Audio Mixer MultiMedia9"
                          : "Incall_Music_2 Audio Mixer MultiMedia9");
    snprintf(s.vtx_bt, sizeof(s.vtx_bt),
             "VoiceMMode%d_Tx Mixer INT_BT_SCO_TX_MMode%d", session, session);
    snprintf(s.vtx_bolero, sizeof(s.vtx_bolero),
             "VoiceMMode%d_Tx Mixer TX_CDC_DMA_TX_3_MMode%d", session, session);

    fprintf(stderr, "[avcrtd] engage_bt: session=%d inject='%s' (BT GSM — live-validation path)\n",
            session, s.inj_mixer);

    // Mark engaged up-front so ANY bail below runs the full MODE_BT teardown in disengage() (clears the
    // placeholder + safely no-ops the reroute restore) instead of the "nothing open" guard early-returning
    // and stranding our mixer hacks — important because step 4 closes the BE-keeper, leaving no open PCM.
    s.engaged = true;

    // 1) bolero PLACEHOLDER mic: DEC0 <- DMIC0 (clocked-but-silent line) -> TX_AIF1 -> TX_CDC_DMA_TX_3.
    mx_enum("TX DMIC MUX0", "DMIC0");
    mx_enum("TX DEC0 MUX", "MSM_DMIC");
    mx_set("TX_AIF1_CAP Mixer DEC0", 1);
    mx_set("MultiMedia1 Mixer TX_CDC_DMA_TX_3", 1);

    // 2) BE-keeper on MM1 -> makes the bolero BE active so the voice-Tx reroute fires voc_set_device_config.
    pcm_config bcfg = make_cfg();
    s.be_keeper = pcm_open(g_card, kCapDevice, PCM_IN, &bcfg);
    if (!s.be_keeper || !pcm_is_ready(s.be_keeper)) {
        fprintf(stderr, "[avcrtd] engage_bt: BE-keeper open failed: %s\n",
                s.be_keeper ? pcm_get_error(s.be_keeper) : "null");
        return false;
    }
    { int16_t tmp[kPeriod]; pcm_read(s.be_keeper, tmp, sizeof(tmp)); }

    // 3) reroute (ORDER CRITICAL): BT off first (voc_disable_device), bolero on last
    //    (voc_set_device_config(bolero)+voc_enable_device) -> modem reads the silent bolero placeholder.
    mx_set(s.vtx_bt, 0);
    mx_set(s.vtx_bolero, 1);
    if (g_patch_present) mx_set(kDecMuteCtl, 0xFF);   // modem hears ONLY our inject

    // 4) close the BE-keeper (the CVP/modem now holds the bolero BE) and RE-POINT MM1 to capture the
    //    freed BT SCO mic (16k mono; the FE SRCs from the 8k/16k SCO BE). Retry the start — the SCO
    //    endpoint may need a moment to free after the reroute.
    pcm_close(s.be_keeper); s.be_keeper = nullptr;
    mx_set("MultiMedia1 Mixer TX_CDC_DMA_TX_3", 0);
    mx_set("MultiMedia1 Mixer INTERNAL_BT_SCO_TX", 1);
    pcm_config ccfg = make_cfg();
    bool started = false;
    for (int attempt = 0; attempt < 12 && g_run.load(); ++attempt) {
        sleep_ms(120);
        if (!mx_any_on(s.vtx_bolero)) {              // reroute didn't hold / call moved -> bail
            fprintf(stderr, "[avcrtd] engage_bt: bolero reroute didn't hold — bail\n");
            return false;
        }
        if (!s.cap) s.cap = pcm_open(g_card, kCapDevice, PCM_IN, &ccfg);
        if (!s.cap || !pcm_is_ready(s.cap)) {
            if (s.cap) { pcm_close(s.cap); s.cap = nullptr; }
            continue;
        }
        int16_t tmp[kPeriod];
        if (pcm_read(s.cap, tmp, sizeof(tmp)) == 0) { started = true; break; }
        pcm_close(s.cap); s.cap = nullptr;           // start failed (EPIPE) — clean retry
    }
    if (!started) {
        fprintf(stderr, "[avcrtd] engage_bt: BT mic capture failed to start — INTERNAL_BT_SCO_TX not "
                        "AP-capturable after reroute (likely needs kernel/AHAL work) — bail\n");
        return false;
    }
    fprintf(stderr, "[avcrtd] engage_bt: BT mic capture started (MM1 <- INTERNAL_BT_SCO_TX)\n");

    // 5) inject path on.
    mx_set(s.inj_mixer, 1);
    pcm_config icfg = make_cfg();
    s.inj = pcm_open(g_card, kInjDevice, PCM_OUT, &icfg);
    if (!s.inj || !pcm_is_ready(s.inj)) {
        fprintf(stderr, "[avcrtd] engage_bt: inj open failed: %s\n",
                s.inj ? pcm_get_error(s.inj) : "null");
        return false;
    }
    mx_set("Playback 23 Volume", 8192);

    s.engaged = true;
    return true;
}

// UNPLUG morph: headset → built-in WITHOUT tearing down the working inject. A headset reroute
// (voice Tx swapped USB→bolero) bypasses the incall_music mix-point so a full disengage+re-engage
// kills the inject (PROVEN on device: post-transition a direct siren didn't reach the far end). So
// instead KEEP voice Tx ← TX_CDC_DMA_TX_3 and the inject PCM, and only: (a) wait for the AHAL to bring
// up the built-in mic decimators after the unplug, (b) extend the codec mute to silence them (+ the
// placeholder DEC0), (c) switch our capture from the dead USB mic to a built-in DEC<spare>←DMIC1.
// Returns false if the AHAL never brought up a built-in mic (caller cleanly disengages → real voice).
bool morph_to_builtin(Session& s) {
    int modem_mask = 0;
    for (int tries = 0; tries < 12 && g_run.load(); ++tries) {
        sleep_ms(150);                          // let the AHAL re-route to the built-in mic
        modem_mask = detect_modem_mask();
        if (modem_mask) break;
    }
    if (!modem_mask) return false;              // no built-in mic came up → caller falls back
    int mute_mask = modem_mask | (1 << 0);      // + the headset placeholder DEC0
    int spare = pick_spare_dec(mute_mask);

    // tear down ONLY the USB capture + BE-keeper — NOT the inject, NOT the voice-Tx (keeps the CVS /
    // incall_music mix alive).
    if (s.usb_cap)   { pcm_close(s.usb_cap);   s.usb_cap = nullptr; }
    if (s.be_keeper) { pcm_close(s.be_keeper); s.be_keeper = nullptr; }

    char dmic[20], decmux[20], aif2[28], aif1[28];
    snprintf(dmic,   sizeof(dmic),   "TX DMIC MUX%d", spare);
    snprintf(decmux, sizeof(decmux), "TX DEC%d MUX",  spare);
    snprintf(aif2,   sizeof(aif2),   "TX_AIF2_CAP Mixer DEC%d", spare);
    snprintf(aif1,   sizeof(aif1),   "TX_AIF1_CAP Mixer DEC%d", spare);
    mx_set(aif1, 0);                            // spare must NEVER feed the modem leg
    mx_enum(dmic, "DMIC1");                     // capture the real bottom mic (AHAL drives its clock)
    mx_enum(decmux, "MSM_DMIC");
    mx_set(aif2, 1);
    mx_set(kCapFromTx3, 0);
    mx_set(kCapFromTx4, 1);
    mx_set(kDecMuteCtl, mute_mask);             // silence the modem's built-in decimators + DEC0

    pcm_config ccfg = make_cfg();
    s.cap = pcm_open(g_card, kCapDevice, PCM_IN, &ccfg);
    if (!s.cap || !pcm_is_ready(s.cap)) {
        if (s.cap) { pcm_close(s.cap); s.cap = nullptr; }
        return false;
    }
    // Now a built-in capture, but the inject (s.inj) + voice-Tx are PRESERVED from the headset flow.
    s.mode       = MODE_BUILTIN;
    s.modem_mask = modem_mask;
    s.mute_mask  = mute_mask;
    s.spare_dec  = spare;
    snprintf(s.spare_aif1, sizeof(s.spare_aif1), "TX_AIF1_CAP Mixer DEC%d", spare);
    snprintf(s.spare_aif,  sizeof(s.spare_aif),  "TX_AIF2_CAP Mixer DEC%d", spare);
    snprintf(s.spare_dmic_mux, sizeof(s.spare_dmic_mux), "TX DMIC MUX%d", spare);
    fprintf(stderr, "[avcrtd] morph headset->built-in: modem=0x%x mute=0x%x spare=%d (inject preserved)\n",
            modem_mask, mute_mask, spare);
    return true;
}

void disengage(Session& s, bool headset_gone = false) {
    if (!s.engaged && !s.cap && !s.inj && !s.usb_cap && !s.be_keeper) return;

    if (s.mode == MODE_BT) {
        // Bluetooth teardown — mirror of the headset path. 'headset_gone' here = BT route/device gone.
        fprintf(stderr, "[avcrtd] disengage_bt: teardown (gone=%d)\n", (int)headset_gone);
        if (g_patch_present) mx_set(kDecMuteCtl, 0);     // un-mute the placeholder DEC(s)
        bool call_up = is_in_call();
        if (s.vtx_bolero[0]) mx_set(s.vtx_bolero, 0);    // always drop our bolero reroute
        // restore the BT uplink only if the call is CONTINUING + BT is still the route (engine toggled
        // off mid-call). On a call-end / BT gone, leave voice Tx for the AHAL (don't strand a dead route).
        if (call_up && !headset_gone && s.vtx_bt[0]) mx_set(s.vtx_bt, 1);
        if (s.inj) { pcm_close(s.inj); s.inj = nullptr; }
        if (s.cap) { pcm_close(s.cap); s.cap = nullptr; }
        if (s.be_keeper) { pcm_close(s.be_keeper); s.be_keeper = nullptr; }
        if (s.inj_mixer[0]) mx_set(s.inj_mixer, 0);
        mx_set("MultiMedia1 Mixer INTERNAL_BT_SCO_TX", 0);   // drop our BT-mic capture tap
        mx_set("TX_AIF1_CAP Mixer DEC0", 0);                 // tear down the silent placeholder DEC
        mx_enum("TX DMIC MUX0", "ZERO");
        mx_set("MultiMedia1 Mixer TX_CDC_DMA_TX_3", 1);      // restore MM1's default codec source
        Session fresh; s = fresh;
        return;
    }

    if (s.mode == MODE_HEADSET) {
        fprintf(stderr, "[avcrtd] disengage_headset: teardown (headset_gone=%d)\n", (int)headset_gone);
        if (g_patch_present) mx_set(kDecMuteCtl, 0);     // un-mute the placeholder DEC
        // Voice-Tx restore is CONDITIONAL on the disengage REASON (NOT find_usb_card — that races the
        // async USB de-registration on unplug and wrongly reports "present"):
        //  - headset_gone (unplug / capture broke): the AHAL is moving the call to the built-in mic.
        //    Do NOT touch the voice Tx (forcing USB-on to a dead card confuses the CVP → additive leak).
        //  - else (call-end / engine-off, headset still there): restore the USB uplink so the call
        //    returns to the headset mic (bolero OFF, USB ON last).
        // Voice Tx is the AHAL's to own. We ALWAYS drop our bolero reroute. We only RESTORE the USB
        // uplink when the call is CONTINUING (engine toggled OFF mid-call) and the headset is still
        // present — so it keeps working with the headset mic. On a CALL-END we set NOTHING (let the
        // AHAL clear voice Tx for the ended call): setting USB here left a DEAD USB route that broke the
        // NEXT call once the headset was unplugged (the user's silence). NEVER restore USB when the
        // headset is gone (dead card).
        bool call_up = is_in_call();
        if (s.vtx_bolero[0]) mx_set(s.vtx_bolero, 0);
        if (call_up && !headset_gone && s.vtx_usb[0] && find_usb_card() >= 0)
            mx_set(s.vtx_usb, 1);
        if (s.inj)       { pcm_close(s.inj);       s.inj = nullptr; }
        if (s.usb_cap)   { pcm_close(s.usb_cap);   s.usb_cap = nullptr; }
        if (s.be_keeper) { pcm_close(s.be_keeper); s.be_keeper = nullptr; }
        if (s.inj_mixer[0]) mx_set(s.inj_mixer, 0);
        mx_set("TX_AIF1_CAP Mixer DEC0", 0);            // tear down our silent placeholder DEC
        mx_enum("TX DMIC MUX0", "ZERO");
        // NOTE: do NOT force "MultiMedia1 Mixer TX_CDC_DMA_TX_3" off here — if the AHAL now feeds the
        // built-in call mic through TX_CDC_DMA_TX_3, clearing it could disturb the live call; the
        // built-in engage/disengage manages MM1 routing itself.
        Session fresh; s = fresh;
        return;
    }

    // built-in (bolero) mode
    fprintf(stderr, "[avcrtd] disengage: unmute + restore\n");
    mx_set(kDecMuteCtl, 0);                       // real voice back
    if (s.inj) { pcm_close(s.inj); s.inj = nullptr; }
    if (s.cap) { pcm_close(s.cap); s.cap = nullptr; }
    if (s.inj_mixer[0]) mx_set(s.inj_mixer, 0);
    if (s.spare_aif[0]) mx_set(s.spare_aif, 0);
    mx_set(kCapFromTx4, 0);
    mx_set(kCapFromTx3, 1);
    // clear the spare decimator's source (whichever we replicated — DMIC or analog SMIC/ADC).
    if (s.spare_dmic_mux[0]) mx_enum(s.spare_dmic_mux, "ZERO");
    if (s.spare_smic_mux[0]) mx_enum(s.spare_smic_mux, "ZERO");
    if (s.spare_dec_mux[0])  mx_enum(s.spare_dec_mux, "MSM_DMIC");
    // If this built-in session was MORPHED from a headset call, the bolero reroute (voice Tx ← bolero)
    // is STILL in place (the morph kept it to preserve the inject). CLEAR it now so it doesn't linger
    // into the next call → otherwise the next call's AHAL sees the route set, never connects the uplink
    // = silence (the regression that needed an audioserver reset). Normal built-in calls never set
    // vtx_bolero → this is a no-op for them. Restore USB only if a headset is somehow still present.
    if (s.vtx_bolero[0]) {
        mx_set(s.vtx_bolero, 0);
        // restore USB only if the call is CONTINUING + the headset is still here (else leave voice Tx
        // for the AHAL — setting a dead USB route breaks the next call).
        if (is_in_call() && s.vtx_usb[0] && find_usb_card() >= 0) mx_set(s.vtx_usb, 1);
        mx_set("TX_AIF1_CAP Mixer DEC0", 0);      // the headset placeholder DEC (silent line)
        mx_enum("TX DMIC MUX0", "ZERO");
    }
    Session fresh;
    s = fresh;
}

bool engine_enabled(ConfigStore& cfg, DspConfig& out) {
    out = cfg.snapshot();
    return out.enabled;
}

// Has the accessory / mic path CHANGED vs what we engaged with? Decides HAMMER (the AHAL re-routed
// the call to a new mic → full rebuild) vs a TRANSIENT blip (same route → just retry). NO hardcode:
// compares the live discovered path to the engaged Session. This is what lets a capture I/O error
// distinguish "headset plugged/unplugged" (→ hammer) from a startup/xrun blip (→ retry, no needless
// audioserver restart / call-start glitch).
//   ACC_SAME    = same accessory + same modem mic decimators
//   ACC_CHANGED = USB plugged/unplugged, OR the codec modem AIF / decimators moved (analog jack
//                 plug/unplug, built-in<->analog)
//   ACC_UNKNOWN = codec uplink momentarily unreadable (mid-transition) — caller decides
enum AccCheck { ACC_SAME, ACC_CHANGED, ACC_UNKNOWN };
AccCheck check_accessory(const Session& s) {
    bool usb_now = (find_usb_card() >= 0);
    if (usb_now != (s.mode == MODE_HEADSET)) return ACC_CHANGED;   // USB headset plugged/unplugged
    if (s.mode == MODE_BUILTIN) {
        int ds = 0, na = 0;
        if (!discover_codec_uplink(ds, na)) return ACC_UNKNOWN;    // uplink gone for a moment
        if (na != s.modem_aif) return ACC_CHANGED;                 // AIF moved (built-in<->analog)
        int dmask = decs_in_aif(na);
        if (dmask && dmask != s.modem_mask) return ACC_CHANGED;    // different modem decimators
    }
    return ACC_SAME;
}

}  // namespace

// Self-contained recovery: clear the codec mute + tear down any leftover spare
// routing, then exit. Used by the supervisor on daemon exit (crash/SIGKILL) and
// for manual recovery — no external tinymix dependency.
int run_unmute() {
    g_card = find_audio_card();
    g_mixer = mixer_open(g_card);
    if (!g_mixer) return 1;
    mx_set(kDecMuteCtl, 0);
    mx_set("MultiMedia1 Mixer TX_CDC_DMA_TX_4", 0);
    mx_set("MultiMedia1 Mixer TX_CDC_DMA_TX_3", 1);
    // Tear down our spare capture decimators. The spare lives in the NON-modem AIF (built-in modem =
    // AIF1 -> spare in AIF2; analog-jack modem = AIF2 -> spare in AIF1). DISCOVER the live modem AIF and
    // clear ONLY the OTHER AIF's decs, so --unmute mid-call never rips the mic off the modem. No call
    // (live=false) -> clear both AIFs (nothing to disturb). This replaces the old fixed AIF2-only clear
    // which would have killed an analog-headset call (whose modem mic IS in AIF2).
    int unm_sess = 0, modem_aif = 0;
    bool live = discover_codec_uplink(unm_sess, modem_aif);
    for (int aif = 1; aif <= 2; ++aif) {
        if (live && aif == modem_aif) continue;          // protect the live modem leg
        for (int i = 0; i < 8; ++i) {
            char n[28]; snprintf(n, sizeof(n), "TX_AIF%d_CAP Mixer DEC%d", aif, i);
            mx_set(n, 0);
        }
    }
    mx_set("Incall_Music Audio Mixer MultiMedia9", 0);
    mx_set("Incall_Music_2 Audio Mixer MultiMedia9", 0);

    // HEADSET-reroute leftover recovery: if a USB headset is present AND the voice Tx was left on the
    // bolero placeholder (our reroute) with USB disconnected, restore USB as the uplink so a crash/
    // SIGKILL mid-headset-call doesn't strand the call mic on the silent bolero. (A real bolero call
    // has NO USB card; a real headset call has USB On — so this ONLY fires on our leftover state.)
    if (find_usb_card() >= 0) {
        for (int n = 1; n <= 2; ++n) {
            char bolero[48], usb[44];
            snprintf(bolero, sizeof(bolero), "VoiceMMode%d_Tx Mixer TX_CDC_DMA_TX_3_MMode%d", n, n);
            snprintf(usb,    sizeof(usb),    "VoiceMMode%d_Tx Mixer USB_AUDIO_TX_MMode%d", n, n);
            if (mx_any_on(bolero) && !mx_any_on(usb)) {
                mx_set(bolero, 0);   // bolero off (voc_disable_device)
                mx_set(usb, 1);      // USB on last (voc_set_device_config(USB)+enable)
            }
        }
        mx_set("TX_AIF1_CAP Mixer DEC0", 0);     // tear down the placeholder DEC
        mx_enum("TX DMIC MUX0", "ZERO");
    }

    // BT-reroute leftover recovery (crash/SIGKILL mid-BT-call): if our MM1 BT-mic tap is still On, the
    // daemon died with the call rerouted to the silent bolero placeholder -> the BT call's uplink is
    // stranded silent. The MM1 BT tap is the UNAMBIGUOUS signal (a normal codec call never sets it, even
    // though it also has bolero On), so restoring on it can't break a real built-in call. Restore the BT
    // uplink (bolero off, BT on last) + tear down our taps.
    if (mx_any_on("MultiMedia1 Mixer INTERNAL_BT_SCO_TX")) {
        for (int n = 1; n <= 2; ++n) {
            char bolero[48], bt[48];
            snprintf(bolero, sizeof(bolero), "VoiceMMode%d_Tx Mixer TX_CDC_DMA_TX_3_MMode%d", n, n);
            snprintf(bt,     sizeof(bt),     "VoiceMMode%d_Tx Mixer INT_BT_SCO_TX_MMode%d", n, n);
            if (mx_any_on(bolero) && !mx_any_on(bt)) {
                mx_set(bolero, 0);   // bolero off (voc_disable_device)
                mx_set(bt, 1);       // BT on last (voc_set_device_config(BT)+enable)
            }
        }
        mx_set("MultiMedia1 Mixer INTERNAL_BT_SCO_TX", 0);
        mx_set("TX_AIF1_CAP Mixer DEC0", 0);
        mx_enum("TX DMIC MUX0", "ZERO");
        mx_set("MultiMedia1 Mixer TX_CDC_DMA_TX_3", 1);
    }

    mixer_close(g_mixer);
    fprintf(stderr, "[avcrtd] --unmute: codec mute cleared + routing restored\n");
    return 0;
}

// Probe: does the running kernel expose the "AVCRT Dec Mute" kcontrol (i.e. is the
// AVCRT kernel patch flashed)? Exit 0 if present, 1 if not. Used by customize.sh.
int run_probe_kcontrol() {
    g_card = find_audio_card();
    struct mixer* m = mixer_open(g_card);
    if (!m) return 1;
    struct mixer_ctl* c = mixer_get_ctl_by_name(m, kDecMuteCtl);
    int ok = (c != nullptr) ? 0 : 1;
    mixer_close(m);
    return ok;
}

// Read-only probe: a missing mixer is an inspection error, not an absent control.
int run_probe_cvp() {
    struct mixer* m = mixer_open(find_audio_card());
    if (!m) return 2;
    const bool present = mixer_get_ctl_by_name(m, kCvpRebuildCtl) != nullptr;
    mixer_close(m);
    return present ? 0 : 1;
}

// ---- afhook auto-injection (the universal AudioFlinger-level voice hook) -----
//
// The afhook .so (libavcrt_afhook.so) pitches EVERY mic capture that flows through
// AudioFlinger's RecordThread — apps, mediaserver, the camera — at the HAL read.
// It can't load itself: it must be ptrace-injected into the audioserver process (a
// non-zygote init service). We drive that here, in the already-root, auto-supervised
// daemon, and set avcrt.afhook.active=1 so the Zygisk hook gates itself off the paths
// afhook now covers (no double-pitch).
//
// SAFETY (this runs on a daily driver): if our injected hook were to crash audioserver,
// it would respawn and we'd re-inject → an audio bootloop. So we DISABLE afhook
// (and clear the flag → Zygisk resumes covering apps) after a few quick post-inject
// restarts, leaving audioserver clean. The inject itself is isolated in a short-lived
// child process (the avcrt_inject binary), so a ptrace glitch can't take down avcrtd.
constexpr const char* kAfhookSo = "/system/lib64/libavcrt_afhook.so";

void setprop_str(const char* key, const char* val) {
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "setprop %s %s", key, val);
    int rc = system(cmd);
    (void)rc;
}

pid_t pidof_one(const char* name) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", name);
    FILE* p = popen(cmd, "r");
    if (!p) return 0;
    pid_t pid = 0;
    if (fscanf(p, "%d", &pid) != 1) pid = 0;   // first pid only (audioserver is singleton)
    pclose(p);
    return pid;
}

bool afhook_in_maps(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "re");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libavcrt_afhook")) { found = true; break; }
    }
    fclose(f);
    return found;
}

// Run the avcrt_inject helper (sibling of this binary) in a child process. Returns 0
// only if it exits 0. Isolated so a ptrace failure never affects the daemon.
int run_injector(pid_t target) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return -1;
    exe[n] = '\0';
    char* slash = strrchr(exe, '/');
    if (!slash) return -1;
    *slash = '\0';
    char inj[600];
    snprintf(inj, sizeof(inj), "%s/avcrt_inject", exe);
    // Self-heal the executable bit: the module's set_perm_recursive lands files at 0644 and a forgotten
    // set_perm on avcrt_inject leaves it non-executable → execl below EACCES'd → "inject did not take".
    // We're root, so just ensure +x (harmless if already correct).
    chmod(inj, 0755);
    char pidstr[16];
    snprintf(pidstr, sizeof(pidstr), "%d", target);
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execl(inj, inj, pidstr, kAfhookSo, (char*)nullptr);
        _exit(127);   // exec failed
    }
    int st = 0;
    if (waitpid(child, &st, 0) < 0) return -1;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
}

void* afhook_monitor(void*) {
    pid_t  last_injected = 0;
    time_t last_inject_t = 0;
    int    strikes       = 0;       // quick post-inject restarts (suspected hook crash)
    bool   disabled      = false;
    const int kCrashWindowSec = 12; // a restart within this of our inject = suspect us
    const int kMaxStrikes     = 3;

    while (g_run.load()) {
        if (disabled) { for (int i = 0; i < 10 && g_run.load(); ++i) sleep_ms(500); continue; }

        pid_t as = pidof_one("audioserver");
        if (as == 0) { sleep_ms(1000); continue; }                  // not up yet

        // ALREADY injected (by us, or a previous avcrtd instance the supervisor respawned
        // over)? Adopt it — re-injecting a second copy would inline-hook the already-patched
        // prologue and corrupt it. This is the steady state.
        if (afhook_in_maps(as)) {
            if (as != last_injected) {
                setprop_str("avcrt.afhook.active", "1");   // make sure the gate flag is set
                last_injected = as;
                if (last_inject_t == 0) last_inject_t = time(nullptr);
            }
            sleep_ms(2000);
            continue;
        }

        // audioserver is NEW and NOT injected (boot, or a restart that dropped our hook).
        time_t now = time(nullptr);
        if (as != last_injected && last_inject_t != 0 && now - last_inject_t < kCrashWindowSec) {
            // it respawned right after we injected → our hook likely crashed it.
            if (++strikes >= kMaxStrikes) {
                setprop_str("avcrt.afhook.active", "0");   // Zygisk resumes covering apps
                fprintf(stderr, "[avcrtd] afhook: audioserver restarted %d× shortly after inject — "
                                "DISABLING afhook (audio left clean)\n", strikes);
                disabled = true;
                continue;
            }
        } else if (as != last_injected) {
            strikes = 0;   // healthy/independent restart
        }

        sleep_ms(1500);   // let audioserver finish init (load libaudioflinger_datapath)
        if (!g_run.load()) break;
        as = pidof_one("audioserver");
        if (as == 0) continue;

        if (run_injector(as) == 0) {
            sleep_ms(300);
            if (afhook_in_maps(as)) {
                setprop_str("avcrt.afhook.active", "1");
                last_injected = as;
                last_inject_t = time(nullptr);
                fprintf(stderr, "[avcrtd] afhook injected into audioserver %d (active)\n", as);
                continue;
            }
        }
        fprintf(stderr, "[avcrtd] afhook inject did not take for audioserver %d — retrying\n", as);
        sleep_ms(2000);
    }
    return nullptr;
}

// The "HAMMER" for an accessory change mid-call. Instead of surgically adapting our routing
// to each plug/unplug (the fragile morph/re-assert web — too many {built-in,analog,USB} ×
// {plug,unplug} combinations, each with different AHAL behavior), we RESET: the caller first
// disengages (clears all our mixer hacks), then this restarts audioserver so the AHAL rebuilds
// the call's routing from scratch on the NEW device. PROVEN on device: the modem call survives;
// the voice session drops ~3-5s then re-establishes. Afterwards the idle loop re-engages from a
// clean slate via detect_mode/engage — which self-gate on the voice route being back, so every
// transition reduces to the already-working fresh-call (static) case. Deterministic, no per-
// transition special cases. The daemon runs as root (magisk context) → setprop ctl.restart works
// (same context as the verified manual test).
// A crude fingerprint of the current accessory/uplink state — to tell a genuine NEW transition
// (different state → its hammer is legit) from a loop (SAME state re-firing → suppress).
int accessory_fp() {
    int usb = (find_usb_card() >= 0) ? 1 : 0;
    int s = 0, aif = 0;
    discover_codec_uplink(s, aif);            // aif 0 (none) / 1 (TX_3) / 2 (TX_4)
    return usb * 100 + aif;
}

static time_t g_last_hammer = 0;
static int    g_last_hammer_fp = -1;
void restart_audioserver() {
    // RATE-LIMIT — but STATE-AWARE. A hammer is the right tool for each DIFFERENT accessory transition
    // (type-c→built-in→analog = 3 legit hammers). The danger is a LOOP: the SAME state re-firing
    // hammers in a tight cycle → brick audio. So: suppress a hammer ONLY if it's the SAME accessory
    // state as the last hammer AND within ~8s (= a re-fire/loop). A DIFFERENT state (a real new
    // transition) is ALWAYS allowed — even right after another hammer. (The pure time-based limit
    // wrongly suppressed the minijack-after-type-c transition's hammer → the CVP latch wasn't cleared
    // → additive.)
    time_t now = time(nullptr);
    int fp = accessory_fp();
    if (g_last_hammer != 0 && now - g_last_hammer < 8 && fp == g_last_hammer_fp) {
        fprintf(stderr, "[avcrtd] HAMMER suppressed (same state fp=%d, %lds ago) — loop guard, settle only\n",
                fp, (long)(now - g_last_hammer));
        for (int i = 0; i < 15 && g_run.load(); ++i) sleep_ms(200);   // ~3s settle, no restart
        return;
    }
    g_last_hammer = now;
    g_last_hammer_fp = fp;
    fprintf(stderr, "[avcrtd] HAMMER: restart audioserver (rebuild call routing for the new device)\n");
    int rc = system("setprop ctl.restart audioserver");
    (void)rc;
    // wait for audioserver to respawn (init service), then settle for the AHAL to re-establish
    // the voice session. detect_mode won't engage until the voice-Tx route is back, so a modest
    // fixed settle is enough; the idle loop covers any remaining lag.
    for (int i = 0; i < 50 && g_run.load(); ++i) {        // up to ~10s for the new pid
        if (pidof_one("audioserver") > 0) { sleep_ms(200); break; }
        sleep_ms(200);
    }
    for (int i = 0; i < 25 && g_run.load(); ++i) sleep_ms(200);   // ~5s settle for the call audio
    resolve_cards();   // the restart may have RENUMBERED the cards — re-resolve lito + reopen g_mixer
}

// In-kernel CVP/CVS vocproc rebuild for the live call — the AVCRT-unified-kernel replacement for the
// "hammer". Writing the "AVCRT CVP Rebuild" kcontrol runs voc_disable_device()+voc_enable_device() on
// the active voice session: a full vocproc teardown+rebuild (re-sends VSS_IVOCPROC_CMD_SET_DEVICE with
// the current Tx port) that FLUSHES a stale CVP Tx-device latch. Unlike the hammer it does NOT restart
// audioserver, so it (a) is much faster, and (b) does NOT re-enumerate the USB sound card — which is
// exactly what made hammering-with-a-USB-card loop, so this finally fixes the direct minijack->type-c
// swap latch. Called while DISENGAGED (no open capture/inject PCMs to disrupt); the idle loop then
// re-engages cleanly on the settled accessory. Caller gates on g_cvp_rebuild_present.
void cvp_rebuild() {
    struct mixer_ctl* ctl = g_mixer ? mixer_get_ctl_by_name(g_mixer, kCvpRebuildCtl) : nullptr;
    if (!ctl || mixer_ctl_set_value(ctl, 0, 1) < 0) {
        fprintf(stderr, "[avcrtd] CVP rebuild FAILED — control missing or kernel rejected request; no audioserver restart\n");
        return;
    }
    fprintf(stderr, "[avcrtd] CVP rebuild (in-kernel vocproc reset — no audioserver restart), request completed\n");
    // let voc_disable_device+voc_enable_device complete (it pauses then restarts the voice path).
    for (int i = 0; i < 12 && g_run.load(); ++i) sleep_ms(100);   // ~1.2s
    resolve_cards();
}

// After a mid-call device change / capture failure: clear our hacks, then recover the RIGHT way:
//  - USB accessory present  → the headset (reroute) flow handles it → just settle and let the idle
//    loop run engage_headset. NEVER hammer with a USB card present: restarting audioserver flickers
//    USB enumeration → engage_headset's usb_cap prime fails ("headset vanished") → built-in fallback
//    → the 0.5s check sees the USB card again → hammer → LOOP (the type-c regression).
//  - no USB card (pure codec built-in<->analog transition) → HAMMER (restart audioserver → AHAL
//    rebuilds the call routing cleanly → idle re-engages the codec flow). Proven for minijack.
void disengage_and_recover(Session& s) {
    bool ext_gone = (s.mode == MODE_HEADSET || s.mode == MODE_BT);
    resolve_cards();            // operate on the current Qualcomm card (indices may have moved)
    disengage(s, ext_gone);
    if (g_cvp_rebuild_present) {
        // AVCRT UNIFIED KERNEL: flush any stale CVP Tx latch with an in-kernel vocproc rebuild — no
        // audioserver restart, no USB re-enumeration → works even with a USB card present. This replaces
        // BOTH the codec hammer AND the no-hammer-with-USB compromise, so EVERY accessory transition
        // (incl. the direct minijack->type-c swap that the hammer couldn't fix) recovers cleanly.
        cvp_rebuild();
        for (int i = 0; i < 6 && g_run.load(); ++i) sleep_ms(200);   // ~1.2s settle, then idle re-engages
    } else if (g_usb_card >= 0) {   // STOCK kernel: resolve_cards set this (guarded != lito)
        fprintf(stderr, "[avcrtd] recover: USB accessory present → headset flow (no hammer)\n");
        for (int i = 0; i < 6 && g_run.load(); ++i) sleep_ms(200);   // ~1.2s settle for the AHAL/USB
    } else {
        restart_audioserver();      // STOCK kernel, pure codec transition
    }
}

int main(int argc, char** argv) {
    // Never let a typo/new probe flag silently start the audio-routing daemon.
    if (argc == 2) {
        if (strcmp(argv[1], "--unmute") == 0) return run_unmute();
        if (strcmp(argv[1], "--probe-kcontrol") == 0) return run_probe_kcontrol();
        if (strcmp(argv[1], "--probe-cvp") == 0) return run_probe_cvp();
        if (strcmp(argv[1], "--list-probes") == 0) {
            puts("--probe-kcontrol\n--probe-cvp");
            return 0;
        }
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--unmute|--probe-kcontrol|--probe-cvp|--list-probes]\n", argv[0]);
        return 2;
    }

    // SINGLE-INSTANCE GUARD. Two daemons fight over pcm 0,0 / 0,23 (capture+inject)
    // → "cannot prepare channel" / "Bad address" → flap → torn audio. A second
    // instance (e.g. a supervisor respawn while the old one is still stuck in a
    // D-state pcm syscall) must NOT start. flock an exclusive lock; if held, exit.
    int lock_fd = open("/data/local/tmp/avcrtd.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "[avcrtd] another instance is running — exiting\n");
        return 0;
    }

    // Install handlers WITHOUT SA_RESTART so a blocked pcm_read/pcm_write returns
    // EINTR on SIGTERM/SIGINT -> the loop breaks and disengage() runs (unmute).
    // Without this, a stalled inject could hang the daemon while the mic stays
    // muted -> dead mic. This is the clean-stop path.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;                       // no SA_RESTART
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    resolve_cards();   // resolve lito + USB by name, open g_mixer (re-resolved on every renumber)
    if (!g_mixer) { fprintf(stderr, "[avcrtd] mixer_open failed\n"); return 1; }
    fprintf(stderr, "[avcrtd] using audio card %u (usb card %d)\n", g_card, g_usb_card);

    // GATING (per-flow, NOT a hard cellular kill-switch):
    //  - BUILT-IN mic GSM clean-replace needs the "AVCRT Dec Mute" kcontrol (AVCRT kernel patch).
    //    Absent on a stock kernel → we can't cleanly mute the codec mic → built-in flow disabled.
    //  - HEADSET (USB/adapter) GSM clean-replace needs NO patch (reroute to bolero + capture the freed
    //    headset mic + incall_music inject, all stock) → works on ANY kernel when a headset is on the call.
    // So the daemon RUNS regardless of the patch and engages whichever flow the live call supports.
    g_patch_present = mixer_get_ctl_by_name(g_mixer, kDecMuteCtl) != nullptr;
    fprintf(stderr, "[avcrtd] AVCRT kernel patch %s -> built-in-mic GSM %s; headset GSM works either way.\n",
            g_patch_present ? "present" : "ABSENT",
            g_patch_present ? "enabled" : "disabled (stock kernel)");
    // The unified patch also adds "AVCRT CVP Rebuild": when present we recover from accessory
    // transitions with a surgical in-kernel vocproc rebuild instead of the audioserver "hammer".
    g_cvp_rebuild_present = mixer_get_ctl_by_name(g_mixer, kCvpRebuildCtl) != nullptr;
    fprintf(stderr, "[avcrtd] CVP-rebuild kcontrol %s -> accessory recovery via %s.\n",
            g_cvp_rebuild_present ? "present" : "absent",
            g_cvp_rebuild_present ? "in-kernel vocproc rebuild" : "audioserver hammer (stock)");

    ConfigStore cfg;
    Engine engine(kRate, kChannels);
    Session s;
    int64_t applied_revision = -1;
    const size_t frames = kPeriod;
    const size_t bytes  = frames * kChannels * sizeof(int16_t);
    int16_t* buf = new int16_t[frames * kChannels];
    int16_t* usbbuf = new int16_t[kUsbPeriod];        // headset 48k read buffer (decimated into buf)

    // Background call-state poller (keeps dumpsys OUT of the audio loop). ALWAYS runs — both the
    // built-in and headset flows need is_in_call().
    pthread_t mon;
    bool mon_ok = pthread_create(&mon, nullptr, call_monitor, nullptr) == 0;

    // afhook auto-injector (universal AudioFlinger-level hook for apps + mediaserver +
    // camera). Runs ALWAYS, independent of the cellular path — it injects into audioserver
    // and self-disables on a crash loop. Keeps the daemon's mic-capture (pcm 0,0) separate.
    pthread_t afmon;
    bool afmon_ok = pthread_create(&afmon, nullptr, afhook_monitor, nullptr) == 0;

    fprintf(stderr, "[avcrtd] service started (idle poll %d ms, patch=%s, afhook=%s)\n",
            kIdlePollMs, g_patch_present ? "yes" : "no", afmon_ok ? "on" : "off");

    DspConfig dc;
    int  end_misses = 0;                 // consecutive "call not up" checks (×~0.5s)
    int  iters_since_check = 0;
    int  cap_err_streak = 0;             // consecutive capture errors (reset on a clean read)
    const int kCheckEvery = 30;          // re-check on/session every ~30 audio iters (~0.5s)

    while (g_run.load()) {
        // ---- engaged: stay in the tight audio loop; only re-check call/engine
        //      state every ~0.5s (NOT every iteration — those reads add jitter), and
        //      require kEndHysteresis consecutive misses before disengaging so a
        //      momentary mCallState/mixer blip doesn't flap us. ----
        if (s.engaged) {
            if (++iters_since_check >= kCheckEvery) {
                iters_since_check = 0;
                bool on = engine_enabled(cfg, dc);
                // While engaged we do NOT re-run detect_mode(): in headset mode our own reroute
                // (bolero on / USB off) would make it misread the mode → flap. Just check the call
                // is still up + the engine is still on.
                bool call_up = is_in_call();

                // DEVICE CHANGE mid-call (accessory plug/unplug where the capture didn't error first
                // — e.g. plug an accessory during a call so the AHAL moves the modem mic). HAMMER:
                // disengage + restart audioserver so the AHAL rebuilds routing on the new device, then
                // the idle loop re-engages clean. Gated on call_up so a call-end isn't seen as a change.
                if (call_up && check_accessory(s) == ACC_CHANGED) {
                    fprintf(stderr, "[avcrtd] device change detected (mode=%d) — disengage + recover\n", s.mode);
                    disengage_and_recover(s);   // hammer (codec) OR headset flow (USB present)
                    end_misses = 0;
                    continue;
                }

                if (!on || !call_up) {
                    if (++end_misses >= 3) {
                        fprintf(stderr, "[avcrtd] call/engine ended (on=%d call=%d) — disengage\n",
                                (int)on, (int)call_up);
                        disengage(s);
                        end_misses = 0;
                        continue;
                    }
                } else {
                    end_misses = 0;
                    // HEADSET: if the AHAL re-applied USB routing (our reroute got wiped → the real
                    // USB mic is back in the uplink = additive), re-assert the reroute.
                    if (s.mode == MODE_HEADSET && s.vtx_usb[0] && mx_any_on(s.vtx_usb)) {
                        fprintf(stderr, "[avcrtd] headset: AHAL re-applied USB uplink — re-asserting reroute\n");
                        mx_set(s.vtx_usb, 0);
                        mx_set(s.vtx_bolero, 1);
                        if (g_patch_present) mx_set(kDecMuteCtl, 0xFF);   // mute ALL codec decs (headset = inject only)
                    }
                    // BT: same re-assert — if the AHAL put the BT SCO mic back on the uplink, re-reroute.
                    if (s.mode == MODE_BT && s.vtx_bt[0] && mx_any_on(s.vtx_bt)) {
                        fprintf(stderr, "[avcrtd] bt: AHAL re-applied BT uplink — re-asserting reroute\n");
                        mx_set(s.vtx_bt, 0);
                        mx_set(s.vtx_bolero, 1);
                        if (g_patch_present) mx_set(kDecMuteCtl, 0xFF);
                    }
                    // BUILT-IN: if the AHAL re-added our spare DEC into the modem leg (AIF1) — the
                    // headset→built-in transition race — our DMIC1-fed spare leaks the REAL voice to
                    // the modem = additive. Pull it back out + re-assert the codec mute (the AHAL
                    // device-switch may have cleared it too). No-op when the route is already clean.
                    if (s.mode == MODE_BUILTIN && s.spare_aif1[0] && mx_get(s.spare_aif1) > 0) {
                        fprintf(stderr, "[avcrtd] built-in: spare DEC leaked into modem leg — clearing + re-muting\n");
                        mx_set(s.spare_aif1, 0);
                        mx_set(kDecMuteCtl, s.mute_mask);
                    }
                }
                // apply preset on revision change
                if (on && dc.revision != applied_revision) {
                    // Copy the WHOLE config (so the Phase-6 effect sub-structs carry through —
                    // otherwise GSM gets only core pitch/formant/gain, no effects), then override
                    // the few fields the cellular inject path needs.
                    avcrt::DspConfig ec = dc;
                    ec.enabled    = true;
                    ec.time_ratio = 1.0;
                    ec.gain_db    = dc.gain_db + kInjectGainCompDb;
                    ec.revision   = dc.revision;
                    engine.update(ec);
                    applied_revision = dc.revision;
                }
            }
            // ---- audio iteration ----
            if (s.mode == MODE_HEADSET) {
                // Drain the BE-keeper (keeps the bolero placeholder powered). BEST-EFFORT — a read
                // error here is NOT "headset gone": tearing down on it left the call rerouted to a
                // dead placeholder = 0 sound (the mid-call-plug bug). Reopen once; if it still fails,
                // DROP it and carry on — the real capture (usb_cap) + incall_music inject deliver the
                // pitched voice regardless. (Seen on a built-in→headset transition: the bolero MM1
                // capture EFAULTs when the voice path takes over TX_CDC_DMA_TX_3.)
                if (s.be_keeper) {
                    int16_t bk[kPeriod];
                    if (pcm_read(s.be_keeper, bk, sizeof(bk)) != 0) {
                        fprintf(stderr, "[avcrtd] be_keeper read err: %s — reopen (non-fatal)\n", pcm_get_error(s.be_keeper));
                        pcm_close(s.be_keeper); s.be_keeper = nullptr;
                        pcm_config bcfg = make_cfg();
                        struct pcm* bk2 = pcm_open(g_card, kCapDevice, PCM_IN, &bcfg);
                        if (bk2 && pcm_is_ready(bk2)) {
                            int16_t t[kPeriod];
                            if (pcm_read(bk2, t, sizeof(t)) == 0) s.be_keeper = bk2;
                            else pcm_close(bk2);
                        } else if (bk2) { pcm_close(bk2); }
                        // s.be_keeper may stay null → we simply stop draining (placeholder stays
                        // configured; the inject still reaches the uplink). Do NOT disengage.
                    }
                }
                // REAL capture: the freed USB headset mic. An error here might be a real UNPLUG or a
                // transient — settle briefly, then decide by card presence: gone (card dropped) →
                // built-in; still present → restore the USB uplink (call stays audible) + retry headset.
                if (pcm_read(s.usb_cap, usbbuf, kUsbPeriod * sizeof(int16_t)) != 0) {
                    sleep_ms(300);                              // let any async USB de-registration finish
                    bool gone = (find_usb_card() < 0);
                    if (gone) {
                        // UNPLUG: recover. disengage + (USB now gone → HAMMER → AHAL rebuilds the call on
                        // the built-in/analog mic; idle re-engages the codec flow). If it was a swap to
                        // ANOTHER USB device, disengage_and_recover sees USB still present → headset flow.
                        fprintf(stderr, "[avcrtd] usb_cap err: %s — headset gone, disengage + recover\n", pcm_get_error(s.usb_cap));
                        disengage_and_recover(s);
                        end_misses = 0; continue;
                    }
                    // transient (headset still present): restore the USB uplink + retry headset.
                    fprintf(stderr, "[avcrtd] usb_cap read err: %s — transient, restore USB + retry\n", pcm_get_error(s.usb_cap));
                    disengage(s, false);
                    end_misses = 0; sleep_ms(500);
                    continue;
                }
                decimate_3to1(usbbuf, buf, frames);     // 48k 768 -> 16k 256
                engine.process(buf, frames);
                if (pcm_write(s.inj, buf, bytes) != 0) {
                    fprintf(stderr, "[avcrtd] inj write err: %s — disengage\n", pcm_get_error(s.inj));
                    disengage(s); end_misses = 0; continue;
                }
                continue;
            }
            // built-in / analog mode
            if (pcm_read(s.cap, buf, bytes) != 0) {
                // Capture died. This is the FAST signal of an accessory unplug/plug (the AHAL pulls the
                // mic out from under our spare capture) — it fires before the ~0.5s device-change check.
                // Distinguish a real change (→ HAMMER: rebuild the call on the new mic) from a startup/
                // xrun blip (→ plain disengage + retry, NO needless audioserver restart / call glitch).
                const char* err = pcm_get_error(s.cap);
                sleep_ms(300);                          // settle so the AHAL's re-route (if any) is visible
                bool force = (++cap_err_streak >= 3);    // route stuck broken (mixer lies) → recover anyway
                if (check_accessory(s) == ACC_SAME && !force) {
                    fprintf(stderr, "[avcrtd] cap read err: %s — transient (same route), disengage + retry\n", err);
                    disengage(s); end_misses = 0; continue;
                }
                fprintf(stderr, "[avcrtd] cap read err: %s — %s, disengage + recover\n",
                        err, force ? "repeated (route stuck)" : "accessory changed");
                disengage_and_recover(s); cap_err_streak = 0; end_misses = 0; continue;
            }
            cap_err_streak = 0;
            engine.process(buf, frames);
            if (pcm_write(s.inj, buf, bytes) != 0) {
                fprintf(stderr, "[avcrtd] pcm_write err: %s — disengage\n", pcm_get_error(s.inj));
                disengage(s); end_misses = 0; continue;
            }
            continue;
        }

        // ---- idle: poll for (engine on && a supported call), then engage once ----
        // IDLE WATCHDOG (runs whether the engine is on/off): clear a STALE voice-Tx reroute that would
        // break the next call. Unambiguous case: voice Tx ← USB_AUDIO_TX while NO USB card is present —
        // a headset call ended (AHAL/daemon left the uplink on USB), then the headset was unplugged → a
        // DEAD USB uplink route lingers → the next call's AHAL can't connect the mic = silence. A real
        // USB call ALWAYS has a USB card, so clearing this when no card is safe. This makes "no call +
        // headset pulled" self-reset, without hard-coding a specific sequence.
        if (find_usb_card() < 0) {
            for (int n = 1; n <= 2; ++n) {
                char usb[44];
                snprintf(usb, sizeof(usb), "VoiceMMode%d_Tx Mixer USB_AUDIO_TX_MMode%d", n, n);
                if (mx_any_on(usb)) {
                    fprintf(stderr, "[avcrtd] idle watchdog: stale voice Tx <- USB (no card) — clearing\n");
                    mx_set(usb, 0);
                }
            }
        }

        bool on = engine_enabled(cfg, dc);
        if (!on) { sleep_ms(kIdlePollMs); continue; }
        // Re-resolve cards every idle poll (lito + USB by name, reopen g_mixer if an index moved at
        // boot/after a hammer) BEFORE picking the flow → engage always operates on the real audio
        // card and USB card. This handles card renumbering when USB is present at boot.
        resolve_cards();
        int session = 0;
        Mode m = detect_mode(session);
        if (m == MODE_NONE) { sleep_ms(kIdlePollMs); continue; }

        bool ok = (m == MODE_BT)      ? engage_bt(s, session)
                : (m == MODE_HEADSET) ? engage_headset(s, session)
                                      : engage(s, session);
        if (!ok) {
            // Failed engage rolled back its routing in disengage (headset: USB uplink restored → the
            // call keeps working with the unchanged headset mic). Back off longer than the idle poll so
            // a persistent failure (e.g. usb_cap that never starts) doesn't churn the call mic ~2×/s.
            disengage(s);
            sleep_ms((m == MODE_HEADSET || m == MODE_BT) ? 1500 : kIdlePollMs);
            continue;
        }
        applied_revision = -1;
        end_misses = 0;
        iters_since_check = 0;
    }

    disengage(s);
    if (mon_ok) pthread_join(mon, nullptr);
    if (afmon_ok) pthread_join(afmon, nullptr);
    delete[] buf;
    delete[] usbbuf;
    if (g_mixer) mixer_close(g_mixer);
    fprintf(stderr, "[avcrtd] service stopped\n");
    return 0;
}
