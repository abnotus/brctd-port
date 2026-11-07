// SPDX-License-Identifier: GPL-3.0-or-later
// The original's UI text, read at run time from the player's own files
// (CLAUDE.md rule 1: the engine source holds no game text).
//
// The release is identified by the SHA-256 of START.EXE and GAME.OVR
// (fixtures.toml); each string is a Pascal string (length byte, then the
// characters) at a file offset recorded for that release. An unidentified
// release is refused. Strings the port adds itself stay in the source.
#pragma once

#include <array>
#include <string>

#include "core/files.hpp"

namespace cotd {

enum class Text {
    PressContinue,   // DS:0EDE, 0653:0C02's prompt
    MenuContinue,    // DS:0F00, WAIT's one-item menu ('~' + the prompt)
    WhatDoYouDo,     // ovl2 CS:11C7, ACTION_MENU
    LogRecord,       // ovl2 CS:2490, LOGBOOK
    LogEntry,        // ovl2 CS:24A1
    LogEnd,          // ovl2 CS:24B4
    XpTeam,          // ovl2 CS:26D6, GIVE_XP to the team
    XpCongrats,      // ovl2 CS:26FB, GIVE_XP to CurChar: + name +
    XpGains,         // ovl2 CS:2706
    PartyKilled,     // ovl2 CS:1E7B, DAMAGE
    DmgIndent,       // ovl15 CS:1D01, ApplyDamage: + name +
    DmgDies,         // ovl15 CS:1D04
    DmgHitFor,       // ovl15 CS:1D0C
    DmgPoints,       // ovl15 CS:1D19
    WallUnable,      // ovl28 CS:0EB1, LoadWalldef's fatal message: + id +
    WallFrom,        // ovl28 CS:0EC1   + Str(DS:4898) +
    WallEnd,         // ovl28 CS:0ECF
    GeoFatal,        // ovl28 CS:1096, LoadGeo's fatal message
    MonsterFatal,    // ovl17 CS:15DE, LoadMonster's fatal message
    RosterName,      // ovl25 CS:20EE, roster header
    RosterAcHp,      // ovl25 CS:20F3
    RosterMinus,     // ovl25 CS:2397, AC sign
    LostItem,        // ovl23 CS:083C, RemoveItem's message for a node not on the record's list
    StatusZero,      // ovl23 CS:2805..2818: status line pieces
    StatusComma,
    StatusSpace,
    StatusColon,
    StatusStar,
    StatusCamping,
    StatusSearch,
    StatusDebug,     // ovl23 CS:280D, the status line under Ctrl+D
    PicNotFound,     // ovl27 CS:02C2, LoadAnimPicture
    Saving,          // 0653:130B, SaveGame's row-24 line
    NoRoom,          // ovl17 CS:217E, SaveGame on a full disk
    DebugThac0,      // ovl22 CS:057D, ovl32 +0x0004's caption for an attack
    DebugSep,        // ovl32 CS:0000
    DebugSlot,       // ovl33 CS:0000, a debug.txt line
    Dir0, Dir1, Dir2, Dir3, Dir4, Dir5, Dir6, Dir7,   // DS:2150 + 3k, facing names
    AdventureMenu,   // DS:08EC, the adventure menu items
    MoveMenu,        // ovl12 CS:0FBD, the move menu's item
    NotHere,         // ovl12 CS:0FB4
    MapMenu,         // ovl12 CS:0FDA, the map's menu items (state 3)
    QuitToDos,       // ovl12 CS:0F72, after saving
    SaveWhich,       // ovl17 CS:2133, slot menu prompt
    SaveSlots,       // ovl17 CS:2145, slot menu items
    YesNoItems,      // ovl25 CS:1F46
    PickSelect,      // ovl18 CS:2EE1, SelectMember's items
    PickView,        // ovl18 CS:2EE8
    Loading,         // 0653:12BC, LoadAnimPicture's row-24 line
    TravelMenu,      // ovl12 CS:0FC2, AdventureInput state 9
    TravelFuel,      // ovl12 CS:0C5D, TravelMove's prompt
    TravelSpace,     // ovl12 CS:0C6F
    TravelDone,      // ovl12 CS:0C71, TravelMove's item
    BandMercury,     // ovl27 CS:0000, DrawPicture's state-9 band
    BandVenus,       // ovl27 CS:0008
    BandEarth,       // ovl27 CS:000E
    BandMars,        // ovl27 CS:0014
    BandAst,   // ovl27 CS:0019
    ScEnemyShip,   // ovl3 CS:0041
    ScLeapsClear,   // ovl3 CS:06EB
    ScTakes,   // ovl3 CS:0709
    ScPointsDmg,   // ovl3 CS:0711
    ScIsDead,   // ovl3 CS:0724
    ScIsDying,   // ovl3 CS:072F
    ScUnconscious,   // ovl3 CS:073A
    ScInjured,   // ovl3 CS:074B
    ScFires,   // ovl3 CS:097F
    ScSAnd,   // ovl3 CS:0987
    ScHitsFor,   // ovl3 CS:098E
    ScPointsDmg2,   // ovl3 CS:0998
    ScMisses,   // ovl3 CS:09AB
    ScDestroyedOn,   // ovl3 CS:0B10
    ScBang,   // ovl3 CS:0B1F
    ScDestroyed,   // ovl3 CS:0B21
    ScHitsFor2,   // ovl3 CS:1020
    ScPointsDmg3,   // ovl3 CS:102A
    ScMisses2,   // ovl3 CS:103D
    ScAttacksWith,   // ovl3 CS:1045
    ScAnd,   // ovl3 CS:1056
    ScBoost,   // ovl3 CS:128C
    ScSucceeds,   // ovl3 CS:12A9
    ScFails,   // ovl3 CS:12B3
    ScTryDodge,   // ovl3 CS:1C4D
    ScAttemptsRam,   // ovl3 CS:1D4F
    ScYourShip,   // ovl3 CS:1D61
    ScDodges,   // ovl3 CS:1D6B
    ScRamOk,   // ovl3 CS:1D83
    ScClosing,   // ovl3 CS:1ED1
    ScRetreating,   // ovl3 CS:1F4E
    ScReloads,   // ovl3 CS:209A
    ScDot,   // ovl3 CS:20A6
    ScEnemyRigs,   // ovl3 CS:267D
    ScIsPilot,   // ovl3 CS:2AA4
    ScWhatDoes,   // ovl3 CS:2AB5
    ScWantToDo,   // ovl3 CS:2AC0
    ScFireTarget,   // ovl3 CS:2ACD
    ScLoad,   // ovl3 CS:2ADA
    ScJuryrig,   // ovl3 CS:2AE0
    ScQuitView,   // ovl3 CS:2AE9
    ScClose,   // ovl3 CS:2AF4
    ScWdr,   // ovl3 CS:2AFB
    ScRam,   // ovl3 CS:2B05
    ScBoard,   // ovl3 CS:2B0A
    ScCommand,   // ovl3 CS:2B11
    ScAid,   // ovl3 CS:2B1A
    ScBoostEngines,   // ovl3 CS:2B1F
    ScSensors,   // ovl3 CS:2B2E
    ScBoardMsg,   // ovl3 CS:2F9C
    ScTakesOver,   // ovl3 CS:2FD4
    ScTargetWhich,   // ovl3 CS:2FEC
    ScRamQ,   // ovl3 CS:3001
    ScQ,   // ovl3 CS:3006
    ScRecedes,   // ovl3 CS:35D6
    ScSelectReload,   // ovl3 CS:2059
    ScQuit,   // ovl3 CS:2074
    ScColon,   // ovl23 CS:1E5E
    ScTons,   // ovl23 CS:1CEC
    ScFeet,   // ovl23 CS:1CF2
    ScWeapons,   // ovl23 CS:23DE
    ScRange,   // ovl23 CS:22DA
    ScSensorsGone,   // ovl23 CS:2132
    ScFieldUnknown,   // ovl23 CS:1CE8, DrawField
    ScFieldPad,       // ovl23 CS:1CEA
    ScAmmo,           // ovl23 CS:23E7, WeaponList's detail header
    ScReloadsHead,    // ovl23 CS:23EC
    SsName,           // ovl23 CS:32C2, ShipStatus
    SsType,           // ovl23 CS:32C8
    SsRace,           // ovl23 CS:32CE
    SsTonnage,        // ovl23 CS:32D4
    SsLength,         // ovl23 CS:32DD
    SsWidth,          // ovl23 CS:32E5
    SsCargo,          // ovl23 CS:32EC
    SsAc,             // ovl23 CS:32F3
    SsCrew,           // ovl23 CS:32F7
    SsSpeed,          // ovl23 CS:32FD
    SsReact,          // ovl23 CS:3304
    SsRange,          // ovl23 CS:330B
    SsRamming,        // ovl23 CS:3312
    SsShort,          // ovl23 CS:331A
    SsMedium,         // ovl23 CS:3320
    SsLong,           // ovl23 CS:3327
    SsExtreme,        // ovl23 CS:332C
    SsMedical,        // ovl23 CS:3334
    ScSensorSpace,    // ovl3 CS:2FEA, the Sensors line
    ScSensorOk,       // ovl3 CS:3009
    ScSensorFail,     // ovl3 CS:3012
    ScSensorTail,     // ovl3 CS:3018
    ScViewItems,      // ovl3 CS:302E, View's menu
    ScJrWeap,         // ovl3 CS:148F, JuryRig's menu (+0x150F)
    ScJrQuit,         // ovl3 CS:1497
    ScJrAttempts,     // ovl3 CS:14DD
    ScJrSucceeds,     // ovl3 CS:14F4
    ScJrFails,        // ovl3 CS:1503
    ChkSpace,         // ovl2 CS:1644 (SKILL_CHECK's message)
    ChkSucceeds,      // ovl2 CS:1646
    ChkParty,         // ovl2 CS:1653
    ChkFails,         // ovl2 CS:1659
    ScPartyShip,   // ovl17 CS:26E1
    SrRepairing,   // ovl5 CS:0C79
    SrHeader,   // ovl5 CS:0C8B
    SrSpace,   // ovl5 CS:0CB2
    SrFixed,   // ovl5 CS:0CB4
    SrPlundered,   // ovl5 CS:0CBB
    SrDone,   // ovl5 CS:0CE5
    SpriteRange,     // ovl27 CS:0C0E, Show3DSprite's fatal message
    ShipName,          // ovl17 CS:26E1, NewGameDefaults' ship
    ItemSpace,         // ovl23 CS:0110, ItemName
    ItemOpen,          // ovl23 CS:0112
    ItemClose,         // ovl23 CS:0114
    TitlePrompt,       // main CS:0000, the title menu's prompt
    TitleItems,        // main CS:0013
    Credit0,           // ovl1 CS:00CF, credits line 0
    Credit1,           // ovl1 CS:00F4, credits line 1
    Credit2,           // ovl1 CS:0106, credits line 2
    Credit3,           // ovl1 CS:0111, credits line 3
    Credit4,           // ovl1 CS:011E, credits line 4
    Credit5,           // ovl1 CS:013B, credits line 5
    Credit6,           // ovl1 CS:015F, credits line 6
    Credit7,           // ovl1 CS:017B, credits line 7
    Credit8,           // ovl1 CS:0189, credits line 8
    Credit9,           // ovl1 CS:0193, credits line 9
    Credit10,          // ovl1 CS:01B2, credits line 10
    Credit11,          // ovl1 CS:01CE, credits line 11
    Credit12,          // ovl1 CS:01DC, credits line 12
    Credit13,          // ovl1 CS:01E6, credits line 13
    Credit14,          // ovl1 CS:0203, credits line 14
    Credit15,          // ovl1 CS:0220, credits line 15
    Credit16,          // ovl1 CS:023F, credits line 16
    Credit17,          // ovl1 CS:025E, credits line 17
    Credit18,          // ovl1 CS:027D, credits line 18
    Credit19,          // ovl1 CS:029D, credits line 19
    Credit20,          // ovl1 CS:02BE, credits line 20
    TeamPrompt,        // ovl13 CS:0117, TeamMenu's ListMenu prompt
    TeamSelect,        // ovl13 CS:012A
    TeamQuit,          // ovl13 CS:0172, EXIT TO DOS
    TeamNotSaved,      // ovl13 CS:017F
    ListNext,          // ovl25 CS:1AE1, ListMenu's bottom-line items
    ListPrev,          // ovl25 CS:1AE7
    ListExit,          // ovl25 CS:1AED
    PickPrompt,        // ovl18 CS:2A9F, PickCharacter
    PickItems,         // ovl18 CS:2AAF
    AddPrompt,         // ovl13 CS:19FB, ADD CHARACTER TO TEAM
    AddItem,           // ovl13 CS:1A0D
    AddMark,           // ovl13 CS:1A12
    LoadWhich,         // ovl17 CS:1A6A, LoadSavedGame's slot menu
    Exists,            // ovl17 CS:0804, CharSave: name + this
    ExistsItems,       // ovl17 CS:0814
    NewFileName,       // ovl17 CS:0830
    PickRace,          // ovl13 CS:06A9, CreateCharacter
    ItemIndent,        // ovl13 CS:06B3
    CgSelect,          // ovl13 CS:06B7
    ListSelect,        // ovl15 CS:17CC (LIST_MENU's ListMenu)
    PickGender,        // ovl13 CS:06BE
    PickClass,         // ovl13 CS:06CA
    Reroll,            // ovl13 CS:06D5
    TrainConscious,    // ovl13 CS:2B5A, TrainCharacter's refusals and question
    TrainMaxLevel,     // ovl13 CS:2B79
    TrainNoXp,         // ovl13 CS:2BA0
    TrainWillBecome,   // ovl13 CS:2BB6
    TrainALevel,       // ovl13 CS:2BC4
    TrainSpace,        // ovl13 CS:2BD1
    TrainAsk,          // ovl13 CS:2BD3
    TrainCongrats,     // ovl13 CS:2BEA
    ModifyRefused,     // ovl13 CS:1102, MODIFY CHARACTER
    ModifyPrompt,      // ovl13 CS:1136
    ModifyItems,       // ovl13 CS:113F
    ModifyCursor,      // ovl13 CS:0F43
    PurgeDrop,         // ovl13 CS:1808, PURGE CHARACTER
    PurgeForever,      // ovl13 CS:180E
    PurgeSure,         // ovl13 CS:1819
    PurgeIncinerate,   // ovl13 CS:1828
    PurgeDot,          // ovl13 CS:1838
    PurgeHitches,      // ovl13 CS:183A
    PurgeRelief,       // ovl13 CS:1854
    ChangeAnimOn,      // ovl12 CS:0B20, CHANGE
    ChangeAnimOff,     // ovl12 CS:0B2D
    SpeedIs,           // ovl12 CS:02EB, CHANGE's Messages
    SpeedOpen,         // ovl12 CS:02FC
    SpeedClose,        // ovl12 CS:02FF
    SpeedFaster,       // ovl12 CS:0304
    SpeedSlower,       // ovl12 CS:030C
    SpeedExit,         // ovl12 CS:0314
    SpeedPrompt,       // ovl12 CS:031A
    OrderPrompt,       // ovl12 CS:07A5, CHANGE's Order
    OrderSelected,     // ovl12 CS:07B3
    NmSpace8,          // ovl23 CS:0A74, NameMessage in state 8
    NmDot8,            // ovl23 CS:0A76
    CharName,          // ovl13 CS:06E4
    SaveChar,          // ovl13 CS:06F5: + name +
    SaveCharEnd,       // ovl13 CS:06FB
    SheetLabel0,       // ovl18 CS:0045, sheet label 0
    SheetLabel1,       // ovl18 CS:004D, sheet label 1
    SheetLabel2,       // ovl18 CS:0051, sheet label 2
    SheetLabel3,       // ovl18 CS:0059, sheet label 3
    SheetLabel4,       // ovl18 CS:005D, sheet label 4
    SheetLabel5,       // ovl18 CS:0065, sheet label 5
    SheetLabel6,       // ovl18 CS:006C, sheet label 6
    SheetLabel7,       // ovl18 CS:0074, sheet label 7
    SheetLabel8,       // ovl18 CS:007B, sheet label 8
    SheetLabel9,       // ovl18 CS:0083, sheet label 9
    SheetLabel10,      // ovl18 CS:008F, sheet label 10
    SheetLabel11,      // ovl18 CS:0097, sheet label 11
    SheetLabel12,      // ovl18 CS:009C, sheet label 12
    SheetLabel13,      // ovl18 CS:00A4, sheet label 13
    SheetLabel14,      // ovl18 CS:00B1, sheet label 14
    SheetLabel15,      // ovl18 CS:00B9, sheet label 15
    SheetLabel16,      // ovl18 CS:00C1, sheet label 16
    SheetLabel17,      // ovl18 CS:00C9, sheet label 17
    SheetLabel18,      // ovl18 CS:00D4, sheet label 18
    DmgD,              // ovl18 CS:061A, the sheet's damage: 'd'
    DmgPlus,           // ovl18 CS:061C
    DmgMinus,          // ovl18 CS:061E
    HpSlash,           // ovl25 CS:2422, DrawHP
    ViewGear,          // ovl18 CS:0F11, VIEW's items
    ViewSkills,        // ovl18 CS:0F17
    ViewTrade,         // ovl18 CS:0F3F
    ViewWpn,           // ovl18 CS:0F46
    ViewExit,          // ovl18 CS:0F50
    NoGeneral,         // ovl19 CS:0C55, AllocateSkills: name + this
    SkAddSub,          // ovl19 CS:0C6D
    SkPrevNext,        // ovl19 CS:0C7B
    SkDone,            // ovl19 CS:0C86
    SkNoCareer,        // ovl19 CS:0C8C
    SkPrereq,          // ovl19 CS:0CA8
    SkMaxLevel,        // ovl19 CS:0CC4
    SkNoGeneralPts,    // ovl19 CS:0CE4
    SkMaxGeneral,      // ovl19 CS:0D01
    SkNoNew,           // ovl19 CS:0D25
    SkCareerUnspent,   // ovl19 CS:0D4B
    SkGeneralUnspent,  // ovl19 CS:0D6D
    SkGeneralTitle,    // ovl19 CS:06C3, view mode title
    SkCareerPts,       // ovl19 CS:06D2
    SkGeneralPts,      // ovl19 CS:06E7
    SkMaxPer,          // ovl19 CS:06FD
    SkColumns,         // ovl19 CS:0710
    SkNeeds,           // ovl19 CS:02EB, a Needs line: this + n +
    SkPointsIn,        // ovl19 CS:02F2   + skill name
    WpnTitle,          // ovl18 CS:0A32, WeaponSpecialize
    WpnTrain,          // ovl18 CS:0A48
    WpnPrompt,         // ovl18 CS:0A4E
    WpnPad,            // ovl18 CS:0A5F
    WpnOpen,           // ovl18 CS:0A61
    WpnClose,          // ovl18 CS:0A64
    WpnMax,            // ovl18 CS:0A66
    IconOld,           // ovl13 CS:2578, IconSelect
    IconReadyAction,   // ovl13 CS:257C
    IconNew,           // ovl13 CS:258B
    IconPrompt,        // ovl13 CS:258F
    IconDone,          // ovl13 CS:25A4
    IconOk,            // ovl13 CS:25A9
    CbBegins,         // ovl8 CS:2417, Combat.SetUp
    CbWildUnknown,    // ovl8 CS:1234, the wild battlefield's unknown zone (+0x126F)
    CbJetFails,       // ovl11 CS:2652, Jetpack's failure (+0x2667)
    CbFlying,         // ovl11 CS:32C4, Aim in mode 2
    CbLand,           // ovl11 CS:32CB, Aim in mode 2's item
    DbgArrow,         // 0653:0000, the debug line 0653:0007
    DbgDots,          // 0653:0003
    CbSpace,          // ovl23 CS:0470, the panel
    CbPanelHp,      // ovl23 CS:0472
    CbAc,             // ovl23 CS:047C
    CbHelpless,       // ovl23 CS:047F
    CbBlinded,        // ovl23 CS:048A
    CbStunned,        // ovl23 CS:0494
    CbStatusIntim,    // ovl23 CS:049E
    CbMove,           // ovl6 CS:08F0, CombatMenu
    CbTargetView,     // ovl6 CS:08F6
    CbMenuIntim,     // ovl6 CS:0903
    CbQuick,          // ovl6 CS:090F
    CbAid,            // ovl6 CS:0916
    CbSprint,         // ovl6 CS:091B
    CbDodge,          // ovl6 CS:0923
    CbJetpack,        // ovl6 CS:094A
    CbWait,           // ovl6 CS:0953
    CbGuard,          // ovl6 CS:0959
    CbEnd,            // ovl6 CS:0960
    CbShort,          // ovl11 CS:2EC8, ShowRange
    CbMedium,         // ovl11 CS:2ED0
    CbLong,           // ovl11 CS:2ED9
    CbRange,          // ovl11 CS:2EE0
    CbRangeSp,        // ovl11 CS:2EE9
    CbTargetItems,    // ovl11 CS:2FC9, TargetMenu
    CbTargetAttack,   // ovl11 CS:2FDB
    CbTargetExit,     // ovl11 CS:2FE3
    CbAim,            // ovl11 CS:2FEF
    CbBackstabs,      // ovl11 CS:02FB, AttackResult
    CbSlays,          // ovl11 CS:0307
    CbAttacks,        // ovl11 CS:0316
    CbBehind,         // ovl11 CS:031E
    CbCruel,          // ovl11 CS:032D
    CbHitting,        // ovl11 CS:0341
    CbPoint,          // ovl11 CS:034E
    CbPoints,         // ovl11 CS:0356
    CbOfDamage,       // ovl11 CS:035F
    CbMisses,         // ovl11 CS:0369
    CbGoesDown,       // ovl11 CS:0374
    CbDying,          // ovl11 CS:037E
    CbKilled,         // ovl11 CS:03AB
    CbIsStunned,      // ovl11 CS:0C27
    CbMakes,          // ovl11 CS:45DB, Battle Tactics
    CbHis,            // ovl11 CS:45E3
    CbHer,            // ovl11 CS:45E7
    CbTacticsRoll,    // ovl11 CS:45EB
    CbAttackAlly,     // ovl11 CS:2DE0, CanAttack
    CbGuardMsg,       // ovl11 CS:2A29, Guard
    CbUsingLeader,    // ovl11 CS:4385, Leadership
    CbLeads,          // ovl11 CS:439F
    CbPeriod,         // ovl11 CS:43A7
    AiFlees,          // ovl7 CS:0000, AI.Turn
    AiGivesUp,     // ovl7 CS:1276, Morale
    AiHeavy,          // ovl7 CS:1633, ChooseWeapon
    AiMoveLeft,       // ovl7 CS:09A3, MoveStep
    CbGotAway,        // ovl11 CS:0A29, Flee
    CbEscapeBlocked,  // ovl11 CS:0A32
    CbUseCursor,      // ovl11 CS:32DD, Aim
    CbAimAttack,      // ovl11 CS:32D1
    CbAimExit,        // ovl11 CS:32D8
    CbMenacing,       // ovl11 CS:422F, the I command
    CbFailsIntim,     // ovl11 CS:4245
    CbIntimMsg,    // ovl11 CS:425B
    CbImmuneMsg,     // ovl22 CS:0B05, Effect.Apply
    WsLaunch,         // ovl11 CS:1614, WeaponSpecial
    WsThrow,          // ovl11 CS:162A
    WsAstray,         // ovl11 CS:163E
    WsAnd,            // ovl11 CS:164D
    WsIt,             // ovl11 CS:1652
    WsChaff,          // ovl11 CS:1655
    WsCover,          // ovl11 CS:1681
    WsIs,             // ovl11 CS:1692
    AreaDmg1,         // ovl22 CS:08D4, AreaDamage
    AreaDmg2,         // ovl22 CS:08DB
    AreaDmgOne,       // ovl22 CS:08EE
    AreaDownMsg,      // ovl22 CS:0907
    AreaDyingMsg,     // ovl22 CS:0911
    AreaDeadMsg,      // ovl22 CS:0940
    HazardMsg,        // ovl22 CS:042A, TerrainHazard
    FxDodge,          // ovl10 CS:02E3, effect 0x19
    CbStunMsg,        // ovl11 CS:0C27, Attack's stun weapon
    FxAirMsg,         // ovl22 CS:0CA8, the area effects' ageing
    DyComa,           // ovl23 CS:30BF, Dying
    AcWiped1,         // ovl5 CS:2097, AfterCombat: the party destroyed
    TkPrompt,         // ovl5 CS:0B06, Take
    TkItems,          // ovl5 CS:0B0D
    TkGearPrompt,     // ovl5 CS:0921, TakeGear
    TkGearSelect,     // ovl5 CS:0928
    TkCreditsLabel,   // ovl21 CS:03DB, TakeCredits
    TkCreditsPad,     // ovl21 CS:03E6
    TkHowMany,        // ovl21 CS:03F5
    TkOverload,       // ovl14 CS:02EC, GiveItem
    RdRefused,        // ovl18 CS:1F03, the ready toggle
    ZgMade,           // ovl11 CS:47A2, ZeroG
    ZgFailed,         // ovl11 CS:47A8
    ZgHis,            // ovl11 CS:47B0
    ZgHer,            // ovl11 CS:47B4
    ZgIts,            // ovl11 CS:47B8
    ZgRoll,           // ovl11 CS:47BC
    FxPoisonA,        // ovl10 CS:044B, effect 0x1C
    FxDizzy,          // ovl10 CS:0457
    FxPoisoned,       // ovl10 CS:053B, effect 0x1E
    FxSpit,           // DS:07BE, effect 0x20 (ovl10 +0x075E)
    AcWiped2,         // ovl5 CS:20B5
    DyPlural,         // ovl23 CS:30D3
    DySingular,       // ovl23 CS:30D9
    DyHead,           // ovl23 CS:30DD
    DyTail,           // ovl23 CS:30EB
    CbBandaged,       // ovl18 CS:2D79, Aid
    CbTreated,        // ovl18 CS:2D85
    CbLabelSprint,      // ovl6 CS:0E3D, Move
    CbLabelDodge,        // ovl6 CS:0E47
    CbMoveAttack,     // ovl6 CS:0E4F
    CbMoveLeft,       // ovl6 CS:0E5B
    CbDone,           // ovl6 CS:0E6A
    CbFlee,           // ovl6 CS:0E70
    CbCantGo,         // ovl6 CS:0E76
    CbRunning,        // ovl6 CS:1226, Bump
    CbWrongWeapon,    // ovl6 CS:1238
    AcPoisoning,      // ovl5 CS:1328, Medical
    AcColumns,        // ovl5 CS:133D
    AcCured,          // ovl5 CS:1364
    AcDyingPoison,    // ovl5 CS:136B
    AcIsCured,        // ovl5 CS:1380
    AcComa,           // ovl5 CS:138A
    AcWounds,         // ovl5 CS:13BE
    AcSpace,          // ovl5 CS:13D0
    AcNoInjuries,     // ovl5 CS:13D2
    AcNoMedics,       // ovl5 CS:13F0
    AcNoneTreated,    // ovl5 CS:140B
    AcHealed,         // ovl5 CS:1434
    AcAntidote,       // ovl5 CS:120F, Antidote
    AcQuestion,       // ovl5 CS:1220
    AcFled,           // ovl5 CS:0747, XPReport
    AcWon,            // ovl5 CS:075A
    AcEach,           // ovl5 CS:076C
    AcXpPoints,       // ovl5 CS:0785
    AcNo,             // ovl5 CS:0798
    AcView,           // ovl5 CS:1BD3, BootyMenu
    AcTake,           // ovl5 CS:1BD9
    AcDivvy,          // ovl5 CS:1BDF
    AcPool,           // ovl5 CS:1BE6
    AcStatus,         // ovl5 CS:1BEC
    AcExit,           // ovl5 CS:1BF4
    AcYesNo,          // ovl5 CS:1C0F
    AcBootyLeft,      // ovl5 CS:1C18
    AcClaim,          // ovl5 CS:1C35
    AcFound,          // DS:0554, XPReport
    AcBooty,          // DS:056A
    MsAim,            // ovl25 CS:0EB0, MenuScreen's "Aim:" test (0x5ADE6)
    GrNoGear,         // ovl18 CS:18A7, VIEW's Gear (ovl18 +0x1968)
    GrReady,          // ovl18 CS:18D5, Gear's items
    GrTrade,          // ovl18 CS:18FB
    GrDrop,           // ovl18 CS:1902
    GrHalve,          // ovl18 CS:1908
    GrJoin,           // ovl18 CS:190F
    GrSell,           // ovl18 CS:1915
    GrTitle,          // ovl18 CS:191B
    GrReadyItem,      // ovl18 CS:1920
    GrHeavy,          // ovl18 CS:192B
    GrYour,           // ovl18 CS:1942, Drop
    GrGone,           // ovl18 CS:1948
    GrDropIt,         // ovl18 CS:195E
    GrUnready,        // ovl18 CS:117F, CanModify (ovl18 +0x1191)
    GrWhom,           // ovl18 CS:20A8, Gear's Trade (ovl18 +0x20C4)
    GrTooMany,     // ovl18 CS:20B9
    GrCantHalve,      // ovl18 CS:21EC, Halve (ovl18 +0x21FD)
    SlGive,           // ovl18 CS:24A4, Sell (ovl18 +0x2594)
    SlCreditsFor,     // ovl18 CS:24B3
    SlWillYou,        // ovl18 CS:24C6
    SlIt,             // ovl18 CS:24D5
    SlThem,           // ovl18 CS:24DA
    SlYour,           // ovl18 CS:24E1
    SlWorthless,      // ovl18 CS:24E7
    SlDispose,        // ovl18 CS:24FC
    SlAreWorthless,   // ovl18 CS:251C
    SlDiscard,        // ovl18 CS:252C
    SlSold,           // ovl18 CS:254D
    SlSons,           // ovl18 CS:2553
    SlRecycle,        // ovl18 CS:2575
    VtAmount,         // ovl18 CS:281C, VIEW's Trade (ovl18 +0x283C)
    VtTradeTo,        // DS:30CA, VIEW's Trade
    PickExit,         // ovl18 CS:2EEE, SelectMember's Exit
    ItYes,            // ovl23 CS:0116, ItemName's ready prefix
    ItNo,             // ovl23 CS:011D
    NamePossessive,   // ovl25 CS:22BB, DrawCharName's possessive
    RestItems,        // ovl12 CS:15B2, TEAM_MENU 9's menu
    RestRests,        // ovl12 CS:15C8
    ShFunds,          // ovl14 CS:0000, the shop's funds lines
    ShPooled,         // ovl14 CS:0010
    ShBuys,           // ovl14 CS:04A7, Buy's message
    ShBuysA,          // ovl14 CS:04AE
    ShNoMoney,        // ovl14 CS:04B7
    AmNoMissile,      // ovl14 CS:0821, Ammo
    AmWeapon,         // ovl14 CS:0839
    AmClipFill,       // ovl14 CS:0840
    AmItems,          // ovl14 CS:084C
    AmClip,           // ovl14 CS:0856
    AmReloaded,       // ovl14 CS:0868
    AmNoMoney,        // ovl14 CS:087E
    AmFull,           // ovl14 CS:088F
    ShItems,          // ovl14 CS:0E3E, the shop's menu
    ShTakeDivvy,      // ovl14 CS:0E57
    ShExit,           // ovl14 CS:0E63
    ShLeaving,        // ovl14 CS:0E68, Exit with pooled money
    ShGoBack,         // ovl14 CS:0EBB
    ShThanks,         // ovl14 CS:0EE6
    ShItemsPrompt,    // DS:3336, Buy's list prompt
    ShBuy,            // DS:333E
    ShSpaces,         // DS:3342, a Buy line's 38 spaces
    Count
};

class GameText {
public:
    // Identifies the release and reads every string; Error if the release
    // isn't one of the known ones or a string doesn't read back cleanly.
    explicit GameText(const GameFiles& files);

    const std::string& operator[](Text t) const { return text_[static_cast<std::size_t>(t)]; }
    const std::string& release() const { return release_; }
    // START.EXE's initial data segment (DS = 0x0DB0 in the unpacked load
    // image, core/exepack.hpp): numeric tables the engine reads at run time.
    const Bytes& data() const { return data_; }
    std::uint8_t ds8(std::size_t off) const { return data_.at(off); }
    std::uint16_t ds16(std::size_t off) const { return le16(&data_.at(off)); }
    std::uint32_t ds32(std::size_t off) const { return le32(&data_.at(off)); }
    // A Pascal string in the data segment (name tables: races DS:1224,
    // classes DS:11D0, skills DS:13EB, ...), at most `max` characters.
    std::string ds_str(std::size_t off, std::size_t max = 255) const;
    // For listings: name, file and offset of each string.

private:
    std::string release_;
    Bytes data_;
    std::array<std::string, static_cast<std::size_t>(Text::Count)> text_;
};

// The release id of the files in the game directory (fixtures.toml), or
// Error naming the files that don't match a known release.
std::string identify_release(const GameFiles& files);

}  // namespace cotd
