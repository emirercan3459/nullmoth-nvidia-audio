// NMHDA: HDMI and DisplayPort audio from NVIDIA cards.
//
// macOS 15's AppleHDAController attaches only to HD Audio controllers listed in its own table of 48-byte entries
// (vendor, device, flags, ...). The NVIDIA entries are Fermi and Kepler parts (10DE:0BE3-0BEE, 0E08, 0E0A, 0E0B, 0E1A,
// 0E1B), so the audio function of any newer card (an RTX 5060 is 10DE:22EB) gets no driver and the card has no sound
// output. OpenCore cannot patch this kext: it lives in the system kernel collection, and OpenCore's kernel patcher
// reports "Not Found" for it.
//
// When AppleHDAController loads, this plugin reads the device id of each NVIDIA audio function (PCI class 0x0403,
// vendor 0x10DE) from the I/O Registry and rewrites one Kepler entry per card to that id, keeping the Kepler entry's
// flags. Nothing else in the controller changes.

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IONotifier.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <mach/kmod.h>
#include <stdarg.h>
#include <sys/sysctl.h>

static const char *kHdaControllerPath[] {
    "/System/Library/Extensions/AppleHDA.kext/Contents/PlugIns/AppleHDAController.kext/Contents/MacOS/AppleHDAController"
};
static const char *kAppleHdaPath[] {
    "/System/Library/Extensions/AppleHDA.kext/Contents/MacOS/AppleHDA"
};
static KernelPatcher::KextInfo kKexts[] {
    { "com.apple.driver.AppleHDAController", kHdaControllerPath, arrsize(kHdaControllerPath), {true}, {}, KernelPatcher::KextInfo::Unloaded },
    { "com.apple.driver.AppleHDA", kAppleHdaPath, arrsize(kAppleHdaPath), {true}, {}, KernelPatcher::KextInfo::Unloaded },
};

// Kepler entries replaced in this order, one per NVIDIA audio function found.
static const uint16_t kKepler[] { 0x0E0A, 0x0E0B, 0x0E1B, 0x0E1A, 0x0E08 };

static uint32_t readU32(IORegistryEntry *e, const char *key) {
    if (!e) return 0xFFFFFFFF;
    if (auto n = OSDynamicCast(OSNumber, e->getProperty(key)))
        return n->unsigned32BitValue();
    auto d = OSDynamicCast(OSData, e->getProperty(key));
    if (!d || d->getLength() < 2) return 0xFFFFFFFF;
    uint32_t v = 0;
    memcpy(&v, d->getBytesNoCopy(), d->getLength() < 4 ? d->getLength() : 4);
    return v;
}

static void setDataU32(IORegistryEntry *e, const char *key, uint32_t val) {
    if (!e) return;
    if (OSData *d = OSData::withBytes(&val, sizeof(val))) {
        e->setProperty(key, d);
        d->release();
    }
}

// NVIDIA audio functions: vendor 0x10DE, class code 0x0403xx (HD Audio). At most arrsize(kKepler) are returned.
static size_t nvidiaAudioIds(uint16_t *ids, size_t max) {
    size_t n = 0;
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    if (!match) return 0;
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (!it) return 0;
    while (auto o = it->getNextObject()) {
        auto pci = OSDynamicCast(IOPCIDevice, o);
        if (!pci) continue;
        if ((readU32(pci, "vendor-id") & 0xFFFF) != 0x10DE) continue;
        if ((readU32(pci, "class-code") & 0xFFFF00) != 0x040300) continue;
        uint16_t dev = (uint16_t)(readU32(pci, "device-id") & 0xFFFF);
        bool seen = false;
        for (size_t i = 0; i < n; i++) seen |= ids[i] == dev;
        if (!seen && n < max) ids[n++] = dev;
    }
    it->release();
    return n;
}

// Lilu keeps its log in its own buffer, so the outcome is also kept in a read-only sysctl, debug.nmhda (the newest
// lines, oldest first), which Send logs and a user can read at any time after boot.
static char gReport[2048];
SYSCTL_STRING(_debug, OID_AUTO, nmhda, CTLFLAG_RD | CTLFLAG_KERN, gReport, sizeof(gReport), "NVIDIA HD Audio table patch");
static void report(const char *, const char *fmt, ...) {
    char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    SYSLOG("nmhda", "%s", line);
    size_t used = strlen(gReport), need = strlen(line) + 3;
    if (used + need >= sizeof(gReport)) { gReport[0] = 0; used = 0; }   // keep the newest lines
    snprintf(gReport + used, sizeof(gReport) - used, "%s%s", used ? " | " : "", line);
}

static IOService *gNvidiaController = nullptr;
static void *gGk10xPinWidgets[4] = { nullptr, nullptr, nullptr, nullptr };
static uint32_t gLastCodecInfo[4] = { 0, 0, 0, 0 };
static void *gNotifiedFb[8] = {};

// Injects connector-type, av-signal-type, and audio-codec-info onto NVRMFramebuffer heads 0..3
static void applyFbProps(IORegistryEntry *fb, uint32_t depIdx) {
    if (!fb || depIdx >= 4) return;
    uint8_t p = (uint8_t)(4 + depIdx);
    uint8_t c = (uint8_t)(8 + depIdx); // GK10X paths: (4,8), (5,9), (6,10), (7,11)
    uint32_t codecInfo = ((uint32_t)p << 16) | ((uint32_t)c << 8) | 0x00u;
    setDataU32(fb, "connector-type", 0x00000800u);
    setDataU32(fb, "av-signal-type", 0x00000008u);
    setDataU32(fb, "audio-codec-info", codecInfo);
    if (gLastCodecInfo[depIdx] != codecInfo) {
        gLastCodecInfo[depIdx] = codecInfo;
        report("fb", "fb%u props set on %s codec-info=0x%06x", depIdx, fb->getName(), codecInfo);
    }
}

static void setupNvidiaFramebufferProps() {
    OSDictionary *match = IOService::serviceMatching("IOFramebuffer");
    if (!match) return;
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (!it) return;
    while (auto o = it->getNextObject()) {
        auto fb = OSDynamicCast(IOService, o);
        if (!fb) continue;
        if (!fb->getProperty("nvda-eld") && strcmp(fb->getName(), "NVRMFramebuffer") != 0) continue;
        uint32_t depIdx = readU32(fb, "IOFBDependentIndex");
        if (depIdx < 4) {
            applyFbProps(fb, depIdx);
        }
    }
    it->release();
}

static IOService *findAppleDisplayChild(IORegistryEntry *entry, int depth = 0) {
    if (!entry || depth > 5) return nullptr;
    if (strcmp(entry->getName(), "AppleDisplay") == 0 || entry->metaCast("AppleDisplay"))
        return OSDynamicCast(IOService, entry);
    OSIterator *it = entry->getChildIterator(gIOServicePlane);
    if (!it) return nullptr;
    IOService *found = nullptr;
    while (auto o = it->getNextObject()) {
        if (auto child = OSDynamicCast(IORegistryEntry, o)) {
            found = findAppleDisplayChild(child, depth + 1);
            if (found) break;
        }
    }
    it->release();
    return found;
}

// probe/start/gfx/fb hooks for AppleHDAController and AppleHDA
static mach_vm_address_t orgProbe, orgStart, orgGfxMatchedHandler, orgReadyToSendFBMsg;
static mach_vm_address_t orgWidgetGK10XInit, orgUnsolictedCachePinSense, orgHandleValidEDID;
static mach_vm_address_t orgInitSupportMCCS, orgMccsGetVol, orgMccsSetVol, orgMccsGetMute, orgMccsSetMute, orgClipOutputSamples;
using t_framebufferEventGate = IOReturn (*)(void *self, void *ref, void *fb, int event, void *info);
static t_framebufferEventGate fnFramebufferEventGate = nullptr;
static void *fnIntialiseMCCSControls = nullptr;

// Track AppleHDAEngineOutputDP instances for software PCM volume/mute scaling
static void *gDpEngines[8] = {};
static void registerDpEngine(void *eng) {
    if (!eng) return;
    for (size_t i = 0; i < arrsize(gDpEngines); i++) {
        if (gDpEngines[i] == eng) return;
        if (!gDpEngines[i]) { gDpEngines[i] = eng; return; }
    }
}
static bool isDpEngine(void *eng) {
    for (size_t i = 0; i < arrsize(gDpEngines); i++) {
        if (gDpEngines[i] == eng) return true;
    }
    return false;
}

static void wrapInitializeSupportForMCCS(void *param0, void *param1) {
    void *self = param1 ? param1 : param0;
    if (!self) return;
    registerDpEngine(self);
    uint8_t *b = (uint8_t *)self;
    b[0x450] = 1; // Volume control enabled
    b[0x451] = 0; // Speaker select disabled
    b[0x452] = 0; // Balance disabled
    b[0x453] = 1; // Mute control enabled
    if (b[0x454] == 0 || b[0x454] == 0xff) {
        b[0x454] = 0xfe; // Default 100% volume (254)
    }
    *(void **)(b + 0x470) = b + 0x470;
    *(void **)(b + 0x478) = b + 0x470;

    int rc = -1;
    if (fnIntialiseMCCSControls) {
        using t_getWl = void *(*)(void *);
        using t_runAction = IOReturn (*)(void *, void *, void *, void *, void *, void *, void *);
        void *wl = ((t_getWl)((*(uintptr_t **)self)[0x688 / 8]))(self);
        if (wl) {
            rc = ((t_runAction)((*(uintptr_t **)wl)[0x1a0 / 8]))(
                wl, fnIntialiseMCCSControls, self, nullptr, nullptr, nullptr, nullptr
            );
        }
    }
    report("vol", "MCCS virtual volume init rc=0x%x vol=%u", rc, b[0x454]);
    if (rc == 0) {
        using t_release = void (*)(void *);
        ((t_release)((*(uintptr_t **)self)[0x28 / 8]))(self);
    }
}

static IOReturn wrapMccsGetVol(void *self, uint32_t, uint8_t *outVol) {
    uint8_t v = self ? *((uint8_t *)self + 0x454) : 0xfe;
    if (v == 0 || v == 0xff) v = 0xfe;
    if (outVol) *outVol = v;
    return kIOReturnSuccess;
}

static IOReturn wrapMccsSetVol(void *self, uint32_t, uint8_t vol) {
    if (self) *((uint8_t *)self + 0x454) = vol;
    return kIOReturnSuccess;
}

static IOReturn wrapMccsGetMute(void *self, uint32_t, bool *outMute) {
    if (outMute) *outMute = self ? (*((uint8_t *)self + 0x455) != 0) : false;
    return kIOReturnSuccess;
}

static IOReturn wrapMccsSetMute(void *self, uint32_t, bool mute) {
    if (self) *((uint8_t *)self + 0x455) = mute ? 1 : 0;
    return kIOReturnSuccess;
}

static IOReturn wrapClipOutputSamples(void *self, const void *mixBuf, void *sampleBuf,
                                      uint32_t firstSampleFrame, uint32_t numSampleFrames,
                                      const void *streamFormat, void *audioStream) {
    IOReturn r = FunctionCast(wrapClipOutputSamples, orgClipOutputSamples)(
        self, mixBuf, sampleBuf, firstSampleFrame, numSampleFrames, streamFormat, audioStream
    );
    if (r == kIOReturnSuccess && self && sampleBuf && streamFormat && numSampleFrames > 0 && isDpEngine(self)) {
        uint8_t *b = (uint8_t *)self;
        uint8_t vol = b[0x454];
        uint8_t muted = b[0x455];
        uint32_t numChannels = *(const uint32_t *)streamFormat;
        uint8_t bitWidth = *((const uint8_t *)streamFormat + 0x0d);
        if (numChannels > 0 && numChannels <= 8) {
            size_t startSample = (size_t)firstSampleFrame * numChannels;
            size_t totalSamples = (size_t)numSampleFrames * numChannels;
            if (muted || vol <= 1) {
                if (bitWidth == 16) {
                    memset((int16_t *)sampleBuf + startSample, 0, totalSamples * sizeof(int16_t));
                } else if (bitWidth == 32) {
                    memset((int32_t *)sampleBuf + startSample, 0, totalSamples * sizeof(int32_t));
                }
            } else if (vol < 0xfe) {
                // Cubic perceptual curve in 16-bit fixed point (scale: 0..65536)
                uint32_t lin = ((uint32_t)vol * 256u) / 254u;
                uint32_t scale = (lin * lin * lin) >> 8;
                if (bitWidth == 16) {
                    int16_t *p = (int16_t *)sampleBuf + startSample;
                    for (size_t i = 0; i < totalSamples; i++) {
                        p[i] = (int16_t)(((int32_t)p[i] * (int32_t)scale) >> 16);
                    }
                } else if (bitWidth == 32) {
                    int32_t *p = (int32_t *)sampleBuf + startSample;
                    for (size_t i = 0; i < totalSamples; i++) {
                        p[i] = (int32_t)(((int64_t)p[i] * (int64_t)scale) >> 16);
                    }
                }
            }
        }
    }
    return r;
}

static void triggerNvidiaFramebufferEvents(void *self, const char *tag) {
    if (!self || self != gNvidiaController || !fnFramebufferEventGate) return;
    uint8_t rdy = *((uint8_t *)self + 0xed0);
    if (!rdy) return;
    for (int i = 0; i < 8; i++) {
        uint8_t *assoc = (uint8_t *)self + 0xf08 + i * 0x1a8;
        void *fb = *(void **)(assoc + 0x00);
        void *sink = *(void **)(assoc + 0x18);
        uint32_t headIdx = *(uint32_t *)(assoc + 0x28);
        if (fb && !sink && gNotifiedFb[i] != fb) {
            gNotifiedFb[i] = fb;
            IOReturn gr = fnFramebufferEventGate(self, (void *)(uintptr_t)headIdx, fb, 2, nullptr);
            sink = *(void **)(assoc + 0x18);
            uint8_t pin = *(assoc + 0x147);
            uint8_t conv = *(assoc + 0x148);
            report("fbgate", "%s[%d] head=0x%x rdy=%u -> 0x%x sink=%d pin=%u conv=%u",
                   tag, i, headIdx, rdy, gr, sink != nullptr, pin, conv);
        }
    }
}

static bool isNvidiaAudio(IOService *p) {
    auto pci = OSDynamicCast(IOPCIDevice, p);
    return pci && (readU32(pci, "vendor-id") & 0xFFFF) == 0x10DE && (readU32(pci, "class-code") & 0xFFFF00) == 0x040300;
}

static IOService *wrapProbe(IOService *self, IOService *provider, SInt32 *score) {
    auto r = FunctionCast(wrapProbe, orgProbe)(self, provider, score);
    if (isNvidiaAudio(provider)) {
        if (r) gNvidiaController = self;
        report("probe", "probe %s -> %s", provider->getName(), r ? "accepted" : "REFUSED");
    }
    return r;
}

static bool wrapStart(IOService *self, IOService *provider) {
    bool isNv = isNvidiaAudio(provider);
    if (isNv) {
        gNvidiaController = self;
        setupNvidiaFramebufferProps();
    }
    bool r = FunctionCast(wrapStart, orgStart)(self, provider);
    if (isNv) {
        report("start", "start %s -> %s", provider->getName(), r ? "ok" : "FAILED");
        // On a GPU audio link (hda-gfx) the controller keeps the codec link down until the GPU's audio driver announces
        // itself with iokit_vendor_specific_msg(0) (0xE3FF8000, sender = that driver); AppleGFXHDA does this for AMD and
        // Intel, and nothing does it for NVIDIA. The GPU function (function 0 of the same slot) stands in as the sender.
        if (r) {
            IOService *gpu = nullptr;
            if (auto parent = provider->getParentEntry(gIOServicePlane))
                if (auto it = parent->getChildIterator(gIOServicePlane)) {
                    while (auto o = it->getNextObject()) {
                        auto pci = OSDynamicCast(IOPCIDevice, o);
                        if (pci && (readU32(pci, "vendor-id") & 0xFFFF) == 0x10DE && (readU32(pci, "class-code") & 0xFF0000) == 0x030000) { gpu = pci; break; }
                    }
                    it->release();
                }
            IOReturn m = gpu ? self->message(0xE3FF8000, gpu, nullptr) : kIOReturnNotFound;
            report("link", "graphics link announce via %s -> 0x%x", gpu ? gpu->getName() : "(no GPU found)", m);
        }
    }
    return r;
}

static void wrapGfxMatchedHandler(void *self, void *ref, IOService *service, IONotifier *notifier) {
    for (IORegistryEntry *p = service; p; p = p->getParentEntry(gIOServicePlane)) {
        uint32_t depIdx = readU32(p, "IOFBDependentIndex");
        if (depIdx < 4 && (p->getProperty("nvda-eld") || strcmp(p->getName(), "NVRMFramebuffer") == 0)) {
            applyFbProps(p, depIdx);
            break;
        }
    }
    setupNvidiaFramebufferProps();
    if (self && notifier && *(IONotifier **)((uint8_t *)self + 0x1c48) == nullptr) {
        *(IONotifier **)((uint8_t *)self + 0x1c48) = notifier;
    }
    FunctionCast(wrapGfxMatchedHandler, orgGfxMatchedHandler)(self, ref, service, notifier);
    if (self == gNvidiaController) {
        void *firstFb = *(void **)((uint8_t *)self + 0xf08);
        report("gfx", "gfxMatch %s fb0=%d", service ? service->getName() : "?", firstFb != nullptr);
        if (firstFb) {
            triggerNvidiaFramebufferEvents(self, "gfx");
        }
    }
}

static IOReturn wrapReadyToSendFBMsg(IOService *self) {
    if (self == gNvidiaController) {
        setupNvidiaFramebufferProps();
        void *firstFb = *(void **)((uint8_t *)self + 0xf08);
        IONotifier *gfxNotifier = *(IONotifier **)((uint8_t *)self + 0x1c48);
        if (!firstFb && gfxNotifier && orgGfxMatchedHandler) {
            if (OSDictionary *match = IOService::serviceMatching("IOFramebuffer")) {
                if (OSIterator *it = IOService::getMatchingServices(match)) {
                    while (auto o = it->getNextObject()) {
                        auto fb = OSDynamicCast(IOService, o);
                        if (!fb || (!fb->getProperty("nvda-eld") && strcmp(fb->getName(), "NVRMFramebuffer") != 0)) continue;
                        if (readU32(fb, "IOFBDependentIndex") < 4) {
                            if (IOService *disp = findAppleDisplayChild(fb)) {
                                FunctionCast(wrapGfxMatchedHandler, orgGfxMatchedHandler)(self, nullptr, disp, gfxNotifier);
                            }
                        }
                    }
                    it->release();
                }
                match->release();
            }
        }
    }
    IOReturn r = FunctionCast(wrapReadyToSendFBMsg, orgReadyToSendFBMsg)(self);
    if (self == gNvidiaController) {
        triggerNvidiaFramebufferEvents(self, "rdy");
    }
    return r;
}

static uint32_t gRawPinSense[4] = { 0, 0, 0, 0 };

static IOReturn wrapUnsolictedCachePinSense(void *widget) {
    IOReturn r = FunctionCast(wrapUnsolictedCachePinSense, orgUnsolictedCachePinSense)(widget);
    if (widget) {
        uint16_t nid = *(uint16_t *)((uint8_t *)widget + 0x0c);
        uint32_t wtype = *(uint32_t *)((uint8_t *)widget + 0x20);
        if (wtype == 4 && nid >= 4 && nid <= 7 && gGk10xPinWidgets[nid - 4] == widget) {
            uint32_t hwSense = *(uint32_t *)((uint8_t *)widget + 0x114);
            gRawPinSense[nid - 4] = hwSense;
            *(uint32_t *)((uint8_t *)widget + 0x114) = hwSense | 0x80000000u;
        }
    }
    return r;
}

static bool wrapWidgetGK10XInit(void *widget, uint16_t nid, void *fg, void *codec) {
    if (nid >= 4 && nid <= 7) {
        gGk10xPinWidgets[nid - 4] = widget;
    }
    bool r = FunctionCast(wrapWidgetGK10XInit, orgWidgetGK10XInit)(widget, nid, fg, codec);
    if (nid >= 4 && nid <= 7 && widget) {
        report("pin", "gk10x nid=%u hwSense=0x%x", nid, gRawPinSense[nid - 4]);
        *(uint32_t *)((uint8_t *)widget + 0x114) |= 0x80000000u;
    }
    return r;
}

static IOReturn wrapHandleValidEDID(void *driver, void *headAssoc) {
    uint8_t pin = headAssoc ? *((uint8_t *)headAssoc + 0x147) : 0;
    uint8_t conv = headAssoc ? *((uint8_t *)headAssoc + 0x148) : 0;
    uint32_t avSig = headAssoc ? *(uint32_t *)((uint8_t *)headAssoc + 0x138) : 0;
    IOReturn r = FunctionCast(wrapHandleValidEDID, orgHandleValidEDID)(driver, headAssoc);
    void *sink = headAssoc ? *(void **)((uint8_t *)headAssoc + 0x18) : nullptr;
    report("edid", "handleValidEDID pin=%u conv=%u sig=0x%x -> 0x%x sink=%d", pin, conv, avSig, r, sink != nullptr);
    return r;
}

static void rematchNvidiaAudio() {
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    if (!match) return;
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (!it) return;
    size_t n = 0;
    while (auto o = it->getNextObject()) {
        auto pci = OSDynamicCast(IOPCIDevice, o);
        if (!pci || (readU32(pci, "vendor-id") & 0xFFFF) != 0x10DE || (readU32(pci, "class-code") & 0xFFFF00) != 0x040300) continue;
        pci->registerService(kIOServiceAsynchronous);
        n++;
    }
    it->release();
    report("nmhda-rematch", "matching restarted on %lu NVIDIA audio function(s)", (unsigned long)n);
}

static void onKextLoaded(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t, size_t) {
    if (index == kKexts[1].loadIndex) {
        report("nmhda-applehda", "AppleHDA loaded (index %lu)", (unsigned long)index);
        fnIntialiseMCCSControls = reinterpret_cast<void *>(
            patcher.solveSymbol(index, "__ZN22AppleHDAEngineOutputDP21intialiseMCCSControlsEv")
        );
        patcher.clearError();
        KernelPatcher::RouteRequest hdaRoutes[] {
            { "__ZN19AppleHDAWidgetGK10X13initForNodeIDEtP8OSObjectS1_", wrapWidgetGK10XInit, orgWidgetGK10XInit },
            { "__ZN14AppleHDAWidget23unsolictedCachePinSenseEv", wrapUnsolictedCachePinSense, orgUnsolictedCachePinSense },
            { "__ZN21AppleHDAHDMI_DPDriver15handleValidEDIDEP27HDAGFXHeadAssociationStruct", wrapHandleValidEDID, orgHandleValidEDID },
            { "__ZN22AppleHDAEngineOutputDP24initializeSupportForMCCSEPvS0_", wrapInitializeSupportForMCCS, orgInitSupportMCCS },
            { "__ZN22AppleHDAEngineOutputDP27mccsGetCurrentSpeakerVolumeEjPh", wrapMccsGetVol, orgMccsGetVol },
            { "__ZN22AppleHDAEngineOutputDP27mccsSetCurrentSpeakerVolumeEjh", wrapMccsSetVol, orgMccsSetVol },
            { "__ZN22AppleHDAEngineOutputDP25mccsGetCurrentSpeakerMuteEjPb", wrapMccsGetMute, orgMccsGetMute },
            { "__ZN22AppleHDAEngineOutputDP25mccsSetCurrentSpeakerMuteEjb", wrapMccsSetMute, orgMccsSetMute },
            { "__ZN14AppleHDAEngine17clipOutputSamplesEPKvPvjjPK20_IOAudioStreamFormatP13IOAudioStream", wrapClipOutputSamples, orgClipOutputSamples },
        };
        if (!patcher.routeMultiple(index, hdaRoutes, arrsize(hdaRoutes)))
            report("route-hda", "could not wrap AppleHDA symbols (err %d)", patcher.getError());
        patcher.clearError();
        return;
    }
    if (index != kKexts[0].loadIndex) return;
    report("nmhda-controller", "AppleHDAController loaded (index %lu)", (unsigned long)index);
    fnFramebufferEventGate = reinterpret_cast<t_framebufferEventGate>(
        patcher.solveSymbol(index, "__ZN18AppleHDAController20framebufferEventGateEPvP13IOFramebufferiS0_")
    );
    patcher.clearError();
    KernelPatcher::RouteRequest routes[] {
        { "__ZN18AppleHDAController5probeEP9IOServicePi", wrapProbe, orgProbe },
        { "__ZN18AppleHDAController5startEP9IOService", wrapStart, orgStart },
        { "__ZN18AppleHDAController17gfxMatchedHandlerEPvP9IOServiceP10IONotifier", wrapGfxMatchedHandler, orgGfxMatchedHandler },
        { "__ZN18AppleHDAController16readyToSendFBMsgEv", wrapReadyToSendFBMsg, orgReadyToSendFBMsg },
    };
    if (!patcher.routeMultiple(index, routes, arrsize(routes)))
        report("route", "could not wrap controller symbols (patcher error %d)", patcher.getError());
    patcher.clearError();
    uint16_t ids[arrsize(kKepler)] {};
    size_t n = nvidiaAudioIds(ids, arrsize(ids));
    if (n == 0) { report("nmhda-result", "no NVIDIA HD Audio function found - nothing to do"); return; }
    bool patched = false;
    for (size_t i = 0; i < n; i++) {
        bool kepler = false;
        for (auto k : kKepler) kepler |= ids[i] == k;
        if (kepler) { report("nmhda-result", "10DE:%04X is already in the controller's table", ids[i]); continue; }
        // entry prefix: vendor (LE), device (LE), flags 0x00000001 as the Kepler entries carry
        const uint8_t find[8] { 0xDE, 0x10, (uint8_t)(kKepler[i] & 0xFF), (uint8_t)(kKepler[i] >> 8), 0x01, 0x00, 0x00, 0x00 };
        const uint8_t repl[8] { 0xDE, 0x10, (uint8_t)(ids[i] & 0xFF), (uint8_t)(ids[i] >> 8), 0x01, 0x00, 0x00, 0x00 };
        KernelPatcher::LookupPatch p { &kKexts[0], find, repl, sizeof(find), 1 };
        patcher.clearError();
        patcher.applyLookupPatch(&p);
        if (patcher.getError() == KernelPatcher::Error::NoError) {
            report("nmhda-result", "HD Audio table: 10DE:%04X now accepted (in place of Kepler 10DE:%04X)", ids[i], kKepler[i]);
            patched = true;
        } else
            report("nmhda-result", "HD Audio table: could not add 10DE:%04X (patcher error %d)", ids[i], patcher.getError());
    }
    // AppleHDAController is usually loaded (for the board's own audio) before Lilu reaches it, so the NVIDIA function was
    // already probed against the unpatched table and refused, and I/O Kit does not probe a device twice by itself. Matching
    // runs again for each NVIDIA audio function now that the table accepts it.
    // With AppleALC loaded, AppleHDAController's probe reads the NVIDIA function as 10DE:9D70 (an Intel controller id), so
    // the card's own id never matches; the next free Kepler entry takes that id too (no NVIDIA device uses it). Measured
    // with this plugin's probe report on an RTX 5060: "ven 10de dev 9d70".
    if (patched && n < arrsize(kKepler)) {
        const uint16_t k = kKepler[n];
        const uint8_t find[8] { 0xDE, 0x10, (uint8_t)(k & 0xFF), (uint8_t)(k >> 8), 0x01, 0x00, 0x00, 0x00 };
        const uint8_t repl[8] { 0xDE, 0x10, 0x70, 0x9D, 0x01, 0x00, 0x00, 0x00 };
        KernelPatcher::LookupPatch p { &kKexts[0], find, repl, sizeof(find), 1 };
        patcher.clearError();
        patcher.applyLookupPatch(&p);
        report("nmhda-result", "HD Audio table: 10DE:9D70 %s (in place of Kepler 10DE:%04X)",
               patcher.getError() == KernelPatcher::Error::NoError ? "accepted" : "NOT added", k);
        patcher.clearError();
    }
    if (patched) rematchNvidiaAudio();
}

static void pluginStart() {
    sysctl_register_oid(&sysctl__debug_nmhda);
    report("nmhda-started", "waiting for AppleHDA");
    lilu.onKextLoadForce(kKexts, arrsize(kKexts), onKextLoaded);
}

static const char *bootargOff[] { "-nmhdaoff" };
static const char *bootargDebug[] { "-nmhdadbg" };
static const char *bootargBeta[] { "-nmhdabeta" };

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff, arrsize(bootargOff),
    bootargDebug, arrsize(bootargDebug),
    bootargBeta, arrsize(bootargBeta),
    KernelVersion::Sequoia, KernelVersion::Tahoe,
    pluginStart
};

// kmutil places a kext in a kernel collection only when it has a _kmod_info symbol
extern "C" kern_return_t NMHDA_kern_start(kmod_info_t *, void *);
extern "C" kern_return_t NMHDA_kern_stop(kmod_info_t *, void *);
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMHDA, "1.0.0", NMHDA_kern_start, NMHDA_kern_stop) }
