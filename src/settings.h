// Central table of named user settings (the future UI and the bridge `cfg` commands both use it).
// Reads are lock-free atomics (safe inside hooks); writes are range-checked, mark the table "dirty" and take effect LIVE
// through the change handler (see features.cpp). Persisted in the [Cinematic] section of CombatMod.ini.
#pragma once
#include "util.h"

enum CfgId {
    SlowMo_Enabled, SlowMo_ScaleMul, SlowMo_RampInMul, SlowMo_RampOutMul, SlowMo_DenyKeys,
    SlowMo_Gimmicks, SlowMo_GimmickKeys, SlowMo_GimmickWindowMs, SlowMo_MaxChainSeconds,
    BlindingFlash_Mode, BlindingFlash_ChainSpeed, BlindingFlash_HitScale, BlindingFlash_HitRamp, BlindingFlash_FinaleScale, BlindingFlash_FinaleRamp, BlindingFlash_FinaleKeys, BlindingFlash_UnlockSpeedUp,
    Reaction_Finale, Reaction_TurningSlash, Reaction_TurningSlashKeys,
    Launch_Enabled, Launch_Mult, Launch_AngleMul, Launch_MinValue, Launch_DenyKeys,
    HitStop_Enabled, HitStop_ScaleMul, HitStop_DurMul,
    UI_PauseGame,
    Triggers_Enabled, Triggers_Override, Triggers_TowerBlast, Triggers_TowerScale, Triggers_TowerIn, Triggers_TowerHold, Triggers_TowerOut, Triggers_TowerCooldownMs,
    CFG_COUNT
};
enum class CfgType { Bool, Float, List };

struct CfgDef {
    const char* key;      // "SlowMo.ScaleMul"
    const char* label;    // short text for the UI
    CfgType type;
    float def, min, max, step;
    const char* defList;  // default of a List setting ("10010,10031,...")
    bool advanced;        // saved in the [Advanced] ini section instead of [Cinematic] (never shown in the panel)
};

const CfgDef& CfgDefOf(int id);
float CfgMaxOf(int id);                              // the CURRENT maximum: the table's, except BlindingFlash.ChainSpeed (1.0 unless the [Advanced] unlock key is on)
int CfgFind(const std::string& key);                 // case-insensitive; -1 if unknown
float CfgGet(int id);                                // lock-free (bool = 0/1)
inline bool CfgBool(int id) { return CfgGet(id) != 0.0f; }
bool CfgSet(int id, float v, std::string& err);      // range-checked; applies live
bool CfgSetStr(const std::string& key, const std::string& value, std::string& err);   // parses by type ("1", "true", "0.5", "1,2,3", "none")
bool CfgSetList(int id, const std::vector<uint32_t>& keys, std::string& err);         // List settings (max 32 keys)
std::vector<uint32_t> CfgList(int id);
bool CfgListHas(int id, uint32_t key);               // lock-free membership test for hooks
bool CfgKeysMatch(int id, const uint32_t keys[4]);   // lock-free: does any entry ("key" / "key:slot") of the list match these 4 SkillKeys?
int CfgListCopy(int id, uint32_t* out, int maxN);    // lock-free, allocation-free copy of the raw entries; returns how many
std::string CfgValueStr(int id);                     // text form (round-trips through CfgSetStr)
void CfgResetAll();
void CfgNotifyNow();                                 // run the change handler (after CfgRestore, which is silent)                                  // defaults, applies live
bool CfgPreset(const std::string& name, std::string& err);   // vanilla | cinematic | extreme
std::string CfgPresetNames();

bool CfgDirty();                                     // changed since the last load/save
bool CfgSave(const std::wstring& path, std::string& err);    // rewrites only the [Cinematic] and [Advanced] sections of an ini file
bool CfgIsLegacyKey(const std::string& key);                 // keys of removed settings (ignored with a log line)
bool CfgLoad(const std::wstring& path, std::string& err);    // missing keys keep their current value
std::string CfgListText();                           // for `cfg list`

// Live application. The handler runs after every change (once per batch).
typedef void (*CfgHandler)();
CfgHandler CfgSetChangeHandler(CfgHandler fn);   // returns the previous handler
struct CfgBatch { CfgBatch(); ~CfgBatch(); };        // group several sets into one notification
struct CfgNotifyOff { CfgNotifyOff(); ~CfgNotifyOff(); };   // suppress notifications entirely (selftest)
struct CfgSnapshot { float v[CFG_COUNT]; std::vector<uint32_t> lists[5]; bool dirty; };   // lists: SlowMo.DenyKeys, SlowMo.GimmickKeys, Launch.DenyKeys, BlindingFlash.FinaleKeys, Reaction.TurningSlashKeys
CfgSnapshot CfgTake();
void CfgRestore(const CfgSnapshot& s);               // does not notify
void CfgNoteSet(const std::string& note);            // handler -> `cfg set` reply (what the live application did)
std::string CfgTakeNote();

std::string CmdCfg(const Args& t);                   // cfg list|get|set|save|load|preset
