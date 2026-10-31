// ALL build-specific layout numbers of the game, in ONE place. Every source file reads them from here (namespace gl); nothing else may contain
// a game-structure offset. When a game patch moves something, change it HERE (and the stand-in test host follows automatically), then run
// the bridge command `layout check` on the live game: it reports which of these numbers still hold.
//
// Current target: game build 26308950 (exe sha256 e130fbc3...a5e2). Previous build 25477059 values are kept as comments.
// Status per group: VERIFIED = confirmed on the live new build; UNVERIFIED = still the old build's value (check with `layout check`).
#pragma once
#include <cstddef>
#include <cstdint>

namespace gl {
constexpr uint32_t kBuild = 26308950;

// ---- time-event data block D  (D = *(u64*)(a3 + 8) of the time_curve_start hook's arguments)                         VERIFIED (build 26308950)
constexpr uintptr_t kEvDataInArgs = 8;            // D = *(a3 + kEvDataInArgs)
constexpr uintptr_t kEvType = 0x00;               // u16 at D+0: the event type id
constexpr uint32_t kTimeEventType = 125;          // 0x7D, the time-scale event type (build 25477059: 123 = 0x7B)
constexpr uintptr_t kEvScale = 0x0C;              // float: time scale                 (25477059: 0x08)
constexpr uintptr_t kEvScale2 = 0x10;             // float: second scale-like float    (25477059: 0x0C)
constexpr uintptr_t kEvA = 0x14;                  // float: timing A                   (25477059: 0x10)
constexpr uintptr_t kEvB = 0x18;                  // float: timing B                   (25477059: 0x14)
constexpr uintptr_t kEvSpan = 0x10;               // bytes read from kEvScale in one go: scale, scale2, A, B must be consecutive floats
static_assert(kEvScale2 == kEvScale + 4 && kEvA == kEvScale + 8 && kEvB == kEvScale + 12, "the time-event floats are read as one 16-byte block");
constexpr uintptr_t kEvSampleFloats = kEvScale;   // telemetry / etrace show 6 floats starting here (diagnostic only; other event types differ)

// ---- central event dispatcher (hook `event_dispatcher`: a1 = dispatcher, a3 -> event)                                  UNVERIFIED (old values)
constexpr uintptr_t kEvTableBase = 16;            // H = *(a1 + kEvTableBase + kEvTableStride * type)
constexpr uintptr_t kEvTableStride = 8;
constexpr uintptr_t kHandlerFnInVtable = 8;       // fn = *(*H + 8)
constexpr uint32_t kEvTableSlots = 256;           // slots enumerated by telemetry

// ---- time controller (sig time_mgr_global = address of a qword): M = *G, P = *M, obj = *(P + kCtlObj[i])               VERIFIED (build 26308950)
constexpr uintptr_t kCtlObj[2] = {0xCD8, 0xCE8};  // (25477059: 0xCC0, 0xCD0)
constexpr int kCtlFloats = 5;                     // f0..f4 = (scale, 1.0, A, B, progress); idle = (-1, -1, 0, 0, 0)
constexpr size_t kCtlBytes = kCtlFloats * 4;
constexpr uintptr_t kCtlPScanLimit = 0x1000;      // `layout check` looks for idle-looking objects in P below this offset

// ---- apply-attack-skill call (hook `apply_attack_skill_to_position`: a1 = owner, a2 = context)                       UNVERIFIED except the function itself
constexpr uintptr_t kApplyS = 0x70;               // S = *(a2 + 0x70) (the skill record; in the new build's code it is loaded into rdi instead of rsi)
constexpr uintptr_t kApplyQ58 = 0x58;             // two qwords captured with every call
constexpr uintptr_t kApplyQ60 = 0x60;
static_assert(kApplyQ60 == kApplyQ58 + 8, "captured as one 16-byte read");
constexpr uintptr_t kApplyO160 = 0x160;           // another object pointer (attacker / owner?)

// ---- skill (component) record S                                                                                      VERIFIED (build 26308950, read live from real records)
// Real records seen: push 7/2 strength 12.5, bomb 60002/2 strength 13, turning slash 10253/1 launch 0/0, 1002/5/1107/1, 1093 ...
constexpr uintptr_t kSKeys = 0x108;               // 4 SkillKeys (u32) at S+0x108, +0x10C, +0x110, +0x114   (25477059: S+0x100..0x10C)   e.g. push record = 7,2,0,0; 1002 record = 1002,5,1107,1
constexpr int kSKeysCaptured = 8;                 // dwords captured from kSKeys (the first 4 are the keys)
constexpr uintptr_t kSHalf = 0x13C;               // float 0.5 just before the block (informational)       (25477059: 0x134)
constexpr uintptr_t kSNeg = 0x140;                // float in [-1, 0): -0.4 / -0.5 on real launch records   (25477059: 0x138)
constexpr uintptr_t kSMark0 = 0x144;              // FLT_MAX (0x7F7FFFFF) marker x3 ...                     (25477059: 0x13C, 0x140, 0x144)
constexpr uintptr_t kSMark1 = 0x148;
constexpr uintptr_t kSMark2 = 0x14C;
// S+0x150, +0x154, +0x158 are new unknown fields on this build (0, 0, -1.0)
constexpr uintptr_t kSStr0 = 0x15C;               // launch strength floats (12.5 / 12.5 push, 13 / 13 bomb) (25477059: 0x148, 0x14C)
constexpr uintptr_t kSStr1 = 0x160;
constexpr uintptr_t kSAng0 = 0x16C;               // launch angle floats (0.436332 = 25 deg, 0.959931 = 55 deg) (25477059: 0x158, 0x15C)
constexpr uintptr_t kSAng1 = 0x170;
constexpr uint32_t kFltMaxBits = 0x7F7FFFFFu;
constexpr size_t kSProbeBytes = 0x1C0;            // bytes of S kept as a "first seen" probe (telemetry s_probe)
constexpr size_t kSScanSpan = kSAng1 + 4 - kSKeys;   // bytes read for one record validation (S+kSKeys .. S+kSAng1+3)
constexpr size_t kSLaunchSpan = kSStr1 + 4 - kSNeg;  // bytes the launch engine reads in one go (S+kSNeg .. S+kSStr1+3)
// the markers are read as one 12-byte pattern, the strengths / angles as consecutive pairs, the engine block as consecutive dwords:
static_assert(kSMark1 == kSMark0 + 4 && kSMark2 == kSMark1 + 4, "the three FLT_MAX markers are one 12-byte pattern");
static_assert(kSStr1 == kSStr0 + 4 && kSAng1 == kSAng0 + 4, "strengths / angles are read as 8-byte pairs");
static_assert(kSNeg < kSMark0 && kSMark2 < kSStr0 && kSLaunchSpan % 4 == 0 && kSLaunchSpan <= 0x40, "launch-engine block");
static_assert(kSScanSpan <= 0x100 && kSAng1 + 4 <= kSProbeBytes && kSKeys + 4 * kSKeysCaptured <= kSNeg, "scan / probe spans");

// ---- the signatures the plugin hooks or reads: every one must resolve EXACTLY ONCE before anything is hooked (an automatic rescan after ~10 s if not)
constexpr const char* kRequiredSigs[] = {"time_curve_start", "apply_attack_skill_to_position", "event_dispatcher", "time_mgr_global"};

// ---- debug-only signatures: nothing in the plugin hooks or reads them, so their absence is NOT an error (selftest `sigs`, `layout check`)
constexpr const char* kOptionalSigs[] = {"freeze_timescale_cmd", "time_event_apply", "render_scale_setter", "time_curve_reset"};

// ---- hit-reaction ids (S key2 = the crowd-control reaction id; verified live on build 26308950)
constexpr uint32_t kReactPushLight = 1104, kReactPushNormal = 1105, kReactPushHard = 1106, kReactKnockDown = 1107, kReactGroundThrow = 1109, kReactRagdoll = 1110, kReactSmoothDown = 1111;
constexpr uintptr_t kSKey2 = kSKeys + 8;           // the third SkillKey of a damage record: its reaction id

// ---- the skill-record layout check (`layout check`) uses these ranges
constexpr uintptr_t kSCheckSpan = 0x200;          // S+0 .. S+0x1FF is searched for the marker pattern when the expected offsets do not match
}  // namespace gl
