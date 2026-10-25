// CBF-style TAS bot for Geometry Dash 2.2081 / Geode 4.x (Windows, macOS, Android)
//
// Verified against geode-sdk/bindings @ bindings/2.2081/GeometryDash.bro
//
// ---------------------------------------------------------------------------
// Fixes applied vs. the original draft (see REVIEW.md for the full list):
//   1. CCCircleWave has NO update(float) in 2.2081 -> hook draw() instead.
//   2. PlayerObject has NO m_lastPosition (that's GameObject) -> removed.
//   3. CCMenuItemExt callbacks take CCMenuItemSpriteExtra*, not CCObject*.
//   4. geode::ScrollLayer/ListView is the correct list widget; the game's
//      CCScrollLayerExt has no bound static create(), and CCContentLayer
//      expects TableViewCell children -> use ListView + GenericListCell.
//   5. Missing closing brace in refreshMacroList().
//   6. ButtonSprite absolute-width misuse ("OFF" in a 24px box).
//   7. Wall-clock timestamps desync on any stall -> bot now runs on a
//      scheduler-driven game clock that freezes while paused/dead/idle.
//   8. Bot no longer stomps the global time scale to 1.0 while Mode::Off.
//   9. Checkpoint bookkeeping: stale pointers cleared, held buttons restored
//      in Play mode too.
//  10. Macro files now carry format version + level ID and are validated.
//  11. getSaveDir() requires "save-data" in mod.json -> use getConfigDir().
// ---------------------------------------------------------------------------

#include <Geode/Geode.hpp>
#include <Geode/DefaultInclude.hpp>
#include <Geode/Enums.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/file.hpp>

#include <Geode/modify/CCCircleWave.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/UILayer.hpp>

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/EndLevelLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/UILayer.hpp>

#include <Geode/ui/ListView.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Cosmetic unlocks (nothing is written to the save file)
// ---------------------------------------------------------------------------
class $modify(BotGameManager, GameManager) {
    bool isIconUnlocked(int id, IconType type) { return true; }
    bool isColorUnlocked(int id, UnlockType type) { return true; }
};

class $modify(BotGameStatsManager, GameStatsManager) {
    bool isItemUnlocked(UnlockType type, int id) { return true; }
};

// Shared settings, declared before the hooks that read them.
namespace bot {
    inline bool disableDeathEffects = true;
}

class $modify(BotMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (auto mod = Mod::get()) {
            bot::disableDeathEffects = mod->getSavedValue<bool>("disableDeathEffects", true);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Bot core
// ---------------------------------------------------------------------------
namespace bot {

    // "BOT3" -- format v3 (v2 was the draft's step/frac/flags/button layout
    // with no level identity; v2 files are intentionally rejected).
    constexpr uint32_t kMagic = 0x33544F42;
    constexpr uint32_t kFormat = 3;
    constexpr uint32_t kMaxActions = 2'000'000;
    constexpr std::uintmax_t kMaxFileSize = 64ull * 1024ull * 1024ull; // 64 MB

    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t step = 0;
        double frac = 0.0;
        uint8_t button = 1;
        bool down = false;
        bool p1 = true;
    };

    struct StepSub {
        int button = 1;
        bool push = false;
        bool p1 = true;
        double frac = 0.0;
    };

    struct RawInput {
        int button = 1;
        bool push = false;
        bool p1 = true;
        double timestamp = 0.0;   // g_botTime at capture (frame granularity)
        double osTs = 0.0;        // OS event timestamp, sub-frame precision
        double refTs = 0.0;       // GJBaseGameLayer::m_timestamp at that instant
        float refDt = 0.f;        // frame length to normalise against
        bool hasOsTs = false;     // false -> fall back to frame granularity
    };

    inline Mode mode = Mode::Off;
    inline std::vector<Action> macro;
    inline int macroLevelID = 0;       // level the loaded/recorded macro belongs to
    inline uint32_t step = 0;
    inline size_t playIndex = 0;
    inline int applying = 0;
    inline int silent = 0;
    inline uint32_t applyStep = 0;
    inline double applyFrac = 0.0;
    inline bool cpLoaded = false;
    inline std::unordered_map<CheckpointObject*, uint32_t> checkpoints;
    inline double recSpeed = 1.0;
    inline float appliedScale = 1.0f;

    inline std::vector<RawInput> pendingRecordInputs;

    // --- sub-frame input timestamps ------------------------------------
    // GD hands the OS event time to UILayer::handleKeypress(key, down, ts) and
    // compares it against GJBaseGameLayer::m_timestamp in its own
    // shouldUseSubstepForButton(). We capture the same value and use the same
    // reference, so frac is exact to double precision -- and unlike GD's own
    // click-between-frames we apply no `dt * 0.5` gate and no
    // buttonIsRelevant() filter, so every click is recorded and any position
    // inside the tick is reachable.
    //
    // Within one frame the OS event batch is dispatched sequentially
    // (handleKeypress -> ... -> handleButton for that key, then the next
    // event), so "most recent OS timestamp" is exactly the event that caused
    // the handleButton call we are in. No FIFO, no key->button mapping needed.
    inline double g_lastOsTs = 0.0;
    inline uint64_t g_lastOsTsFrame = 0;   // only valid for the frame that set it
    inline uint64_t g_frameCounter = 0;

    // Frame reference captured in the scheduler hook, before anything runs:
    // GD's own frame origin plus the unscaled frame length.
    inline double g_frameTs = 0.0;
    inline float g_frameRawDt = 0.f;

    // Reference snapshot taken at the instant of the click (see
    // markOsTimestamp for why it cannot be sampled in the scheduler hook).
    inline double g_lastRefTs = 0.0;
    inline float g_lastRefDt = 0.f;

    // Diagnostics: if the two time bases ever disagree, frac saturates at the
    // clamp and these show it immediately in the pause-menu status line.
    inline double g_fracMin = 1.0;
    inline double g_fracMax = 0.0;
    inline uint32_t g_fracClamped = 0;
    inline uint32_t g_fracSamples = 0;

    // --- game clock -------------------------------------------------------
    // Driven by the dt handed to CCScheduler::update (unscaled by the
    // scheduler's own time scale). It only advances while a level is actually
    // being simulated, so stalls, alt-tab and the pause menu cannot desync a
    // recording the way a wall clock does.
    inline double g_botTime = 0.0;
    inline bool g_clockActive = false;
    inline double g_stepStartTime = 0.0;

    inline bool inputThisStep = false;
    inline bool p1Split = false;
    inline bool p2Split = false;
    inline bool midStep = false;
    inline CCPoint p1Pos = { 0.f, 0.f };
    inline CCPoint p2Pos = { 0.f, 0.f };
    inline float rotationDelta = 0.f;

    inline bool isPlayLayer(GJBaseGameLayer* l) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == l;
    }

    // Is the pause menu up?
    //
    // getChildByType<PauseLayer> walks every child of PlayLayer doing a typeinfo
    // cast, and isLevelActive() is consulted twice per frame, so the negative answer
    // (the common case during a run) is cached. The cache invalidates itself the
    // moment the child count changes, which is exactly when a PauseLayer would
    // have been added or removed -- so it needs no extra hooks and cannot go
    // stale.
    inline bool g_pauseCached = false;
    inline int g_pauseCachedChildCount = -1;

    inline bool isPaused() {
        auto pl = PlayLayer::get();
        if (!pl) return false;
        int const count = static_cast<int>(pl->getChildrenCount());
        if (!g_pauseCached || count != g_pauseCachedChildCount) {
            g_pauseCached = pl->getChildByType<PauseLayer>(0) != nullptr;
            g_pauseCachedChildCount = count;
        }
        return g_pauseCached;
    }

    // Is the level still on screen and interactive (i.e. NOT in the pause menu
    // and NOT finished)? Deliberately ignores m_playerDied: the death / respawn
    // gap is still "in the level", and the record speed must survive it.
    inline bool isLevelActive() {
        if (isPaused()) return false;
        auto pl = PlayLayer::get();
        if (!pl) return false;
        // Only reached while unpaused, so at most once per completed level.
        if (pl->getChildByType<EndLevelLayer>(0)) return false;
        return true;
    }

    inline void restoreAudioPitch() {
        auto fae = FMODAudioEngine::sharedEngine();
        if (!fae || !fae->m_backgroundMusicChannel) return;
        fae->m_backgroundMusicChannel->setPitch(1.0f);
    }

    inline void updateAudioPitch() {
        auto fae = FMODAudioEngine::sharedEngine();
        if (!fae || !fae->m_backgroundMusicChannel) return;

        // Pitch follows the speedhack, so it too must survive death.
        bool shouldSlow = (mode == Mode::Record) && isLevelActive() && appliedScale > 0.001f;
        float target = std::clamp(shouldSlow ? appliedScale : 1.0f, 0.01f, 2.0f);

        float cur = 1.0f;
        fae->m_backgroundMusicChannel->getPitch(&cur);
        if (std::fabs(cur - target) > 1e-4f) {
            fae->m_backgroundMusicChannel->setPitch(target);
        }
    }

    inline void resetEngine() {
        pendingRecordInputs.clear();
        g_stepStartTime = 0.0;
        g_lastOsTs = 0.0;
        g_lastOsTsFrame = 0;
        g_lastRefTs = 0.0;
        g_lastRefDt = 0.f;
        // NOTE: the frac calibration counters are deliberately NOT reset here.
        // resetEngine() runs on every death / practice restart, and those are
        // exactly the moments you still want the accumulated readout for.
        inputThisStep = false;
        p1Split = false;
        p2Split = false;
        midStep = false;
        rotationDelta = 0.f;
        // `step` is deliberately not touched: PlayLayer owns it.
    }

    inline void resetFracStats() {
        g_fracMin = 1.0;
        g_fracMax = 0.0;
        g_fracClamped = 0;
        g_fracSamples = 0;
    }

    inline void applyClickFlags() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (mode == Mode::Record || mode == Mode::Play) {
            // The bot does its own sub-step placement; make sure the game's
            // click-snapping helpers cannot fight it.
            pl->m_clickBetweenSteps = false;
            pl->m_clickOnSteps = false;
        }
        else {
            auto gm = GameManager::get();
            pl->m_clickBetweenSteps = gm->getGameVariable("0177");
            pl->m_clickOnSteps = gm->getGameVariable("0176");
        }
    }

    inline void setTimeScale(float s) {
        if (std::fabs(s - appliedScale) < 1e-6f) return;
        g_stepStartTime = 0.0;
        appliedScale = s;
        CCDirector::sharedDirector()->getScheduler()->setTimeScale(s);
        updateAudioPitch();
    }

    inline void setMode(Mode m) {
        mode = m;
        applyClickFlags();
        resetEngine();
        resetFracStats();
        if (mode == Mode::Off) {
            setTimeScale(1.0f);
            restoreAudioPitch();
        }
        else {
            updateAudioPitch();
        }
    }

    inline void recordRawInput(int button, bool push, bool isPlayer1) {
        RawInput raw;
        raw.button = button;
        raw.push = push;
        raw.p1 = isPlayer1;
        raw.timestamp = g_botTime;
        // Frame-scoped: a timestamp captured two frames ago must not leak
        // into this click (e.g. a keyboard press followed by a touch that
        // carries no timestamp of its own).
        raw.osTs = g_lastOsTs;
        raw.refTs = g_lastRefTs;
        raw.refDt = g_lastRefDt;
        raw.hasOsTs = (g_lastOsTsFrame == g_frameCounter);
        pendingRecordInputs.push_back(raw);
    }

    // Remember the OS event time for the click we are about to see.
    //
    // We snapshot GJBaseGameLayer::m_timestamp *here*, at the instant of the
    // click, rather than reusing the value captured in onFrameStart(). That
    // matters: the OS event batch is dispatched from the message pump BEFORE
    // CCDirector::drawScene() runs, so a value sampled in the scheduler hook
    // would already belong to the next frame and would shift every frac by one
    // tick. Sampled at click time this is byte-for-byte the same reference GD
    // feeds into shouldUseSubstepForButton(), which is what makes the two time
    // bases agree without any calibration.
    inline void markOsTimestamp(double ts) {
        g_lastOsTs = ts;
        g_lastOsTsFrame = g_frameCounter;
        g_lastRefTs = 0.0;
        g_lastRefDt = g_frameRawDt;
        if (auto pl = PlayLayer::get()) g_lastRefTs = pl->m_timestamp;
    }

    // Sub-frame position of an OS event inside the current tick, in [0,1].
    // Same arithmetic GD uses in shouldUseSubstepForButton():
    //     (command.m_timestamp - m_timestamp) / dt
    // Returns -1.0 when there is no usable OS timestamp, so the caller can
    // fall back to frame granularity instead of silently recording 0.
    inline double osTsToFrac(RawInput const& in) {
        if (!in.hasOsTs || in.refDt <= 1e-9f || in.refTs <= 0.0) return -1.0;
        double f = (in.osTs - in.refTs) / static_cast<double>(in.refDt);
        ++g_fracSamples;
        if (f < 0.0 || f > 1.0) ++g_fracClamped;
        f = std::clamp(f, 0.0, 0.9999999);
        g_fracMin = std::min(g_fracMin, f);
        g_fracMax = std::max(g_fracMax, f);
        return f;
    }

    inline double parseSpeedString(std::string const& str) {
        std::string s = str;
        for (char& c : s) if (c == ',') c = '.';
        double whole = 0.0, frac = 0.0, div = 1.0;
        bool inFrac = false, hasDigits = false;
        for (char c : s) {
            if (c >= '0' && c <= '9') {
                hasDigits = true;
                if (!inFrac) whole = whole * 10.0 + (c - '0');
                else { div *= 10.0; frac += (c - '0') / div; }
            }
            else if (c == '.' && !inFrac) inFrac = true;
            else if (c == ' ') continue;
            else break;
        }
        if (!hasDigits) return 1.0;
        return std::clamp(whole + frac, 0.01, 2.0);
    }

    // getConfigDir() needs no "save-data" declaration in mod.json, unlike
    // getSaveDir(), which throws without it.
    inline std::filesystem::path getMacroDir() {
        auto mod = Mod::get();
        if (!mod) return std::filesystem::path();
        std::error_code ec;
        auto dir = mod->getConfigDir(true) / "macros";
        std::filesystem::create_directories(dir, ec);
        if (ec) return mod->getConfigDir(true);
        return dir;
    }

    inline std::string generateTimestampName() {
        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        std::tm tmStruct{};
#ifdef GEODE_IS_WINDOWS
        localtime_s(&tmStruct, &timeT);
#else
        localtime_r(&timeT, &tmStruct);
#endif
        return fmt::format(
            "macro_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}",
            tmStruct.tm_year + 1900, tmStruct.tm_mon + 1, tmStruct.tm_mday,
            tmStruct.tm_hour, tmStruct.tm_min, tmStruct.tm_sec
        );
    }

    inline std::string sanitizeFileName(std::string const& raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            unsigned char uc = static_cast<unsigned char>(c);
            bool invalid = uc < 0x20
                || c == '\\' || c == '/' || c == ':' || c == '*'
                || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
            out.push_back(invalid ? '_' : c);
        }
        size_t start = out.find_first_not_of(' ');
        if (start == std::string::npos) return "";
        size_t end = out.find_last_not_of(' ');
        out = out.substr(start, end - start + 1);
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
        if (out.size() > 80) out.resize(80);

        std::string upper;
        upper.reserve(out.size());
        for (char c : out) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        static char const* reserved[] = {
            "CON","PRN","AUX","NUL",
            "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
            "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"
        };
        for (auto const* r : reserved) {
            if (upper == r) { out = "_" + out; break; }
        }
        return out;
    }

    inline bool saveMacro(std::filesystem::path const& path,
                          std::vector<Action> const& actions,
                          int levelID) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        uint32_t magic = kMagic;
        uint32_t format = kFormat;
        int32_t lid = static_cast<int32_t>(levelID);
        uint32_t count = static_cast<uint32_t>(actions.size());
        out.write(reinterpret_cast<char const*>(&magic), 4);
        out.write(reinterpret_cast<char const*>(&format), 4);
        out.write(reinterpret_cast<char const*>(&lid), 4);
        out.write(reinterpret_cast<char const*>(&count), 4);
        for (auto const& a : actions) {
            uint8_t flags = static_cast<uint8_t>((a.down ? 1 : 0) | (a.p1 ? 2 : 0));
            uint8_t button = a.button;
            out.write(reinterpret_cast<char const*>(&a.step), 4);
            out.write(reinterpret_cast<char const*>(&a.frac), 8);
            out.write(reinterpret_cast<char const*>(&flags), 1);
            out.write(reinterpret_cast<char const*>(&button), 1);
        }
        return static_cast<bool>(out);
    }

    inline bool loadMacro(std::filesystem::path const& path,
                          std::vector<Action>& actions,
                          int& levelID) {
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        if (ec || size > kMaxFileSize) return false;

        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        uint32_t magic = 0, format = 0, count = 0;
        int32_t lid = 0;
        in.read(reinterpret_cast<char*>(&magic), 4);
        in.read(reinterpret_cast<char*>(&format), 4);
        in.read(reinterpret_cast<char*>(&lid), 4);
        in.read(reinterpret_cast<char*>(&count), 4);
        if (!in || magic != kMagic || format != kFormat || count > kMaxActions) return false;

        std::vector<Action> tmp;
        tmp.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Action a;
            uint8_t flags = 0, button = 1;
            in.read(reinterpret_cast<char*>(&a.step), 4);
            in.read(reinterpret_cast<char*>(&a.frac), 8);
            in.read(reinterpret_cast<char*>(&flags), 1);
            in.read(reinterpret_cast<char*>(&button), 1);
            if (!in) return false;
            a.down = (flags & 1) != 0;
            a.p1 = (flags & 2) != 0;
            a.button = button;
            tmp.push_back(a);
        }
        actions = std::move(tmp);
        levelID = static_cast<int>(lid);
        return true;
    }

    inline int currentLevelID() {
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level) return 0;
        return static_cast<int>(pl->m_level->m_levelID);
    }

    inline void applyInput(PlayLayer* pl, StepSub const& s, uint32_t st, double frac) {
        ++applying;
        applyStep = st;
        applyFrac = std::clamp(frac, 0.0, 1.0);
        // GJBaseGameLayer::handleButton(bool down, int button, bool isPlayer1)
        pl->handleButton(s.push, s.button, s.p1);
        --applying;
    }

    inline void silentReleaseAll(PlayLayer* pl) {
        if (!pl) return;
        ++silent;
        for (int b = 1; b <= 3; ++b) {
            pl->handleButton(false, b, true);
            pl->handleButton(false, b, false);
        }
        --silent;
    }

    // Re-derive the play cursor and the set of physically held buttons from
    // the macro prefix, then force the player objects to match.
    inline void syncPlayback(PlayLayer* pl) {
        if (!pl) return;
        auto it = std::lower_bound(
            macro.begin(), macro.end(), step,
            [](Action const& a, uint32_t s) { return a.step < s; }
        );
        playIndex = static_cast<size_t>(it - macro.begin());

        bool held[4][2] = {};
        for (size_t i = 0; i < playIndex; ++i) {
            auto const& a = macro[i];
            if (a.button < 4) held[a.button][a.p1 ? 0 : 1] = a.down;
        }

        silentReleaseAll(pl);

        ++silent;
        for (int b = 1; b <= 3; ++b) {
            if (held[b][0]) pl->handleButton(true, b, true);
            if (held[b][1]) pl->handleButton(true, b, false);
        }
        --silent;
    }

    // Called from the CCScheduler hook, before the scheduler scales dt.
    inline void onFrameStart(float rawDt) {
        // Snapshot GD's own frame origin and the unscaled frame length before
        // anything runs this frame. Clicks dispatched during this frame are
        // measured against exactly these two numbers.
        ++g_frameCounter;
        g_frameRawDt = rawDt;
        if (auto plf = PlayLayer::get()) g_frameTs = plf->m_timestamp;

        // `simulating` gates the input clock; `levelActive` gates the speedhack.
        // They differ on purpose: while dead the level is still active (so the
        // record speed must stay applied), but nothing is being simulated (so
        // the click clock must not advance and stale clicks must be dropped).
        bool const levelActive = (mode != Mode::Off) && isLevelActive();
        auto pl = PlayLayer::get();
        bool const simulating = levelActive && pl && !pl->m_playerDied && !pl->m_hasCompletedLevel;

        if (simulating && !g_clockActive) {
            // (Re)started simulating: drop anything typed while we were frozen
            // and force a resync of the step window.
            pendingRecordInputs.clear();
            g_stepStartTime = 0.0;
        }
        else if (!simulating && g_clockActive) {
            pendingRecordInputs.clear();
        }
        g_clockActive = simulating;

        if (simulating) {
            // rawDt is the director's unscaled delta; the scheduler applies
            // m_fTimeScale to it right after we return.
            g_botTime += std::clamp(static_cast<double>(rawDt), 0.0, 0.25);
        }

        // FIX: the speedhack no longer drops to 1.0 on death. In Record the
        // scale is held for as long as the level is on screen -- death, respawn
        // and practice restarts included -- and only released for the pause
        // menu, the end-of-level screen and leaving the level.
        //
        // Still only touch the global time scale while we own it, so the bot
        // does not fight other mods (or itself) while switched off.
        if (mode == Mode::Record) {
            setTimeScale(levelActive ? static_cast<float>(recSpeed) : 1.0f);
        }
        else if (appliedScale != 1.0f) {
            setTimeScale(1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Death / spawn visual effects
// ---------------------------------------------------------------------------
// FIX: CCCircleWave in 2.2081 does not declare update(float) at all (it uses
// updatePosition(float) plus a tween action), so `void update(float)` could not
// be hooked and `CCCircleWave::update(dt)` would not compile. draw() is a real
// declared override, so that is what we suppress.
class $modify(BotCircleWave, CCCircleWave) {
    void draw() {
        if (bot::disableDeathEffects && bot::mode != bot::Mode::Off) return;
        CCCircleWave::draw();
    }
};

// ---------------------------------------------------------------------------
// Frame driver
// ---------------------------------------------------------------------------
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        bot::onFrameStart(dt);
        CCScheduler::update(dt);
        bot::updateAudioPitch();
    }
};

// ---------------------------------------------------------------------------
// Input interception
// GJBaseGameLayer::handleButton(bool down, int button, bool isPlayer1) -- this
// is the only declaration in 2.2081 (PlayLayer does not override it).
// ---------------------------------------------------------------------------
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        using namespace bot;

        if (mode == Mode::Off || silent > 0 || applying > 0 || !isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (mode == Mode::Record) {
            // Swallow the real click; it gets re-injected at the right
            // sub-step from PlayerObject::update.
            recordRawInput(button, down, isPlayer1);
            return;
        }

        // Mode::Play: human input is ignored on purpose.
    }
};

// ---------------------------------------------------------------------------
// Sub-frame input timestamps
//
// GD already plumbs the OS event time all the way down to the gameplay layer --
// that is exactly what its own click-between-frames uses. We just read it.
//
// Both of these are real (non-inline, address-bound) functions in 2.2081:
//     UILayer::handleKeypress(enumKeyCodes, bool down, double timestamp)  win 0x4cd830
//     UILayer::processUINodeTouch(GJUITouchEvent, int, CCPoint, GJUINode*, double timestamp)  win 0x4ce5f0
//
// Deliberately NOT hooked:
//   * GJBaseGameLayer::queueButton -- declared `win inline`, its body lives in
//     bindings/2.2081/inline/GJBaseGameLayer.cpp, so Geode compiles a private
//     copy into the mod and a hook would never see the game's own calls.
//   * UILayer::keyDown/keyUp -- the bindings note that UILayer inherits
//     CCKeyboardDelegate twice and that this "ended up breaking addresser when
//     trying to hook it". handleKeypress is the funnel anyway and is safe.
//   * UILayer::ccTouchBegan -- real, but cocos2d::CCTouch carries no timestamp
//     in 2.2081 (m_nId, m_startPointCaptured, m_startPoint, m_point,
//     m_prevPoint and nothing else), so there is nothing sub-frame to read.
//     Direct gameplay touches therefore fall back to frame granularity.
// ---------------------------------------------------------------------------
class $modify(BotUILayer, UILayer) {
    void handleKeypress(cocos2d::enumKeyCodes key, bool down, double timestamp) {
        bot::markOsTimestamp(timestamp);
        UILayer::handleKeypress(key, down, timestamp);
    }

    bool processUINodeTouch(GJUITouchEvent event, int touchID, cocos2d::CCPoint position,
                            GJUINode* node, double timestamp) {
        bot::markOsTimestamp(timestamp);
        return UILayer::processUINodeTouch(event, touchID, position, node, timestamp);
    }
};

// ---------------------------------------------------------------------------
// Sub-step slicing
// ---------------------------------------------------------------------------
class $modify(BotPlayerObject, PlayerObject) {
    void playSpawnEffect() {
        this->setVisible(true);
        if (bot::disableDeathEffects && bot::mode != bot::Mode::Off) return;
        PlayerObject::playSpawnEffect();
    }

    void playDeathEffect() {
        if (bot::disableDeathEffects && bot::mode != bot::Mode::Off) return;
        PlayerObject::playDeathEffect();
    }

    void update(float stepDelta) {
        using namespace bot;
        auto pl = PlayLayer::get();
        if (!pl || midStep || stepDelta <= 1e-9f || mode == Mode::Off) {
            PlayerObject::update(stepDelta);
            if (mode == Mode::Off && pl && this == pl->m_player1) ++step;
            return;
        }

        // Only player 1 drives the step counter / slicing.
        if (this != pl->m_player1) {
            if (!inputThisStep || this != pl->m_player2) {
                PlayerObject::update(stepDelta);
            }
            return;
        }

        uint32_t const cur = step;
        std::vector<StepSub> subs;

        float const scale = (appliedScale > 0.0001f) ? appliedScale : 1.0f;
        // stepDelta already carries the scheduler time scale, so dividing it
        // back out gives the real (wall) length of this physics step.
        double const stepDuration = static_cast<double>(stepDelta) / static_cast<double>(scale);
        double const now = g_botTime;

        if (g_stepStartTime <= 0.0 || (now - g_stepStartTime) > (stepDuration * 4.0)) {
            g_stepStartTime = now - stepDuration;
        }

        double const stepStartTime = g_stepStartTime;
        double const stepEndTime = stepStartTime + stepDuration;

        if (mode == Mode::Play) {
            while (playIndex < macro.size() && macro[playIndex].step <= cur) {
                auto const& a = macro[playIndex++];
                StepSub s;
                s.button = a.button;
                s.push = a.down;
                s.p1 = a.p1;
                s.frac = (a.step < cur) ? 0.0 : std::clamp(a.frac, 0.0, 1.0);
                subs.push_back(s);
            }
        }
        else { // Record
            std::vector<RawInput> remaining;
            remaining.reserve(pendingRecordInputs.size());
            for (auto const& in : pendingRecordInputs) {
                if (in.timestamp <= stepEndTime) {
                    StepSub s;
                    s.button = in.button;
                    s.push = in.push;
                    s.p1 = in.p1;

                    // Prefer the OS event timestamp: that is what gives us a
                    // real sub-frame position instead of "start of tick".
                    double osFrac = osTsToFrac(in);
                    if (osFrac >= 0.0) {
                        s.frac = osFrac;
                    }
                    else {
                        double rel = in.timestamp - stepStartTime;
                        s.frac = std::clamp(rel / stepDuration, 0.0, 0.9999);
                    }
                    subs.push_back(s);

                    Action a;
                    a.step = cur;
                    a.frac = s.frac;
                    a.button = static_cast<uint8_t>(s.button);
                    a.down = s.push;
                    a.p1 = s.p1;
                    macro.push_back(a);
                }
                else {
                    remaining.push_back(in);
                }
            }
            pendingRecordInputs.swap(remaining);
        }

        inputThisStep = !subs.empty();

        if (subs.empty()) {
            p1Split = false;
            p2Split = false;
            PlayerObject::update(stepDelta);
            g_stepStartTime = stepEndTime;
            ++step;
            return;
        }

        std::stable_sort(subs.begin(), subs.end(), [](StepSub const& a, StepSub const& b) {
            return a.frac < b.frac;
        });

        // Keep sub-steps strictly ordered and non-degenerate.
        for (size_t i = 1; i < subs.size(); ++i) {
            if (subs[i].frac <= subs[i - 1].frac + 1e-5) {
                subs[i].frac = std::min(0.9999, subs[i - 1].frac + 1e-5);
            }
        }

        PlayerObject* p2 = pl->m_player2;
        bool const isDual = pl->m_gameState.m_isDualMode && p2 != nullptr;
        bool const p1StartedOnGround = this->m_isOnGround;
        bool const p2StartedOnGround = p2 ? p2->m_isOnGround : false;

        p1Pos = this->getPosition();
        p2Pos = p2 ? p2->getPosition() : CCPoint{ 0.f, 0.f };

        p1Split = true;
        p2Split = isDual;

        midStep = true;
        double lastFrac = 0.0;

        for (size_t i = 0; i < subs.size(); ++i) {
            auto const& s = subs[i];
            double const subFactor = std::max(0.0, s.frac - lastFrac);
            float const substepDelta = static_cast<float>(stepDelta * subFactor);
            rotationDelta = substepDelta;

            if (substepDelta > 1e-9f) {
                PlayerObject::update(substepDelta);
                // Undo the "just landed" bookkeeping the first partial update
                // can produce, otherwise the split frame eats/creates a jump.
                if (i == 0 && ((this->m_yVelocity < 0.0) != (this->m_isUpsideDown != false))) {
                    this->m_isOnGround = p1StartedOnGround;
                }
                pl->checkCollisions(this, substepDelta, false);
                PlayerObject::updateRotation(substepDelta);

                if (p2Split && p2) {
                    p2->update(substepDelta);
                    if (i == 0 && ((p2->m_yVelocity < 0.0) != (p2->m_isUpsideDown != false))) {
                        p2->m_isOnGround = p2StartedOnGround;
                    }
                    pl->checkCollisions(p2, substepDelta, true);
                    p2->updateRotation(substepDelta);
                }
            }

            applyInput(pl, s, cur, s.frac);
            lastFrac = s.frac;
        }

        double const remainingFactor = std::max(0.0, 1.0 - lastFrac);
        float const remainingDelta = static_cast<float>(stepDelta * remainingFactor);
        rotationDelta = remainingDelta;

        if (remainingDelta > 1e-9f) {
            PlayerObject::update(remainingDelta);
            pl->checkCollisions(this, remainingDelta, false);
            PlayerObject::updateRotation(remainingDelta);

            if (p2Split && p2) {
                p2->update(remainingDelta);
                pl->checkCollisions(p2, remainingDelta, true);
                p2->updateRotation(remainingDelta);
            }
        }

        midStep = false;
        p1Split = false;
        p2Split = false;
        g_stepStartTime = stepEndTime;
        ++step;
    }

    // While a step is being split we already advanced rotation by exactly the
    // sub-step delta, so swallow the outer updateRotation call instead of
    // rotating twice.
    // FIX: the original also wrote `this->m_lastPosition`, but PlayerObject has
    // no such member (m_lastPosition lives on GameObject) -> removed.
    void updateRotation(float t) {
        auto pl = PlayLayer::get();
        if (pl && bot::mode != bot::Mode::Off && !bot::midStep) {
            if (this == pl->m_player1 && bot::p1Split) {
                PlayerObject::updateRotation(bot::rotationDelta);
                return;
            }
            if (this == pl->m_player2 && bot::p2Split) {
                PlayerObject::updateRotation(bot::rotationDelta);
                return;
            }
        }
        PlayerObject::updateRotation(t);
    }
};

// ---------------------------------------------------------------------------
// Level + checkpoint controller
// ---------------------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bot::setMode(bot::Mode::Off);
        bot::step = 0;
        bot::playIndex = 0;
        bot::checkpoints.clear();
        bot::macro.clear();
        bot::macroLevelID = 0;

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        if (auto mod = Mod::get()) {
            bot::disableDeathEffects = mod->getSavedValue<bool>("disableDeathEffects", true);
            bot::recSpeed = mod->getSavedValue<double>("recSpeed", 1.0);
        }

        bot::applyClickFlags();

        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            Notification::create(
                "Disable 'Click Between Frames': it conflicts with the bot",
                NotificationIcon::Warning, 4.f
            )->show();
        }
        return true;
    }

    void onQuit() {
        bot::setMode(bot::Mode::Off);
        bot::resetEngine();
        bot::checkpoints.clear();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        bot::cpLoaded = false;
        PlayLayer::resetLevel();
        bot::resetEngine();
        bot::updateAudioPitch();

        if (this->m_player1) this->m_player1->setVisible(true);
        if (this->m_player2) this->m_player2->setVisible(true);

        if (bot::cpLoaded) return; // loadFromCheckpoint already fixed everything up

        bot::step = 0;
        bot::playIndex = 0;
        // Those CheckpointObject pointers are gone after a full reset.
        bot::checkpoints.clear();
        if (bot::mode == bot::Mode::Record) {
            bot::macro.clear();
            bot::macroLevelID = bot::currentLevelID();
        }
        if (bot::mode != bot::Mode::Off) bot::silentReleaseAll(this);
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (cp) bot::checkpoints[cp] = bot::step;
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        bot::cpLoaded = true;
        bot::resetEngine();
        bot::updateAudioPitch();

        if (this->m_player1) this->m_player1->setVisible(true);
        if (this->m_player2) this->m_player2->setVisible(true);

        auto it = bot::checkpoints.find(cp);
        if (it == bot::checkpoints.end()) {
            bot::step = 0;
            bot::playIndex = 0;
            if (bot::mode != bot::Mode::Off) {
                bot::setMode(bot::Mode::Off);
                Notification::create("Bot: unknown checkpoint, switched OFF", NotificationIcon::Warning)->show();
            }
            return;
        }

        bot::step = it->second;
        bot::g_stepStartTime = 0.0;
        bot::pendingRecordInputs.clear();

        if (bot::mode == bot::Mode::Record) {
            uint32_t const s = bot::step;
            auto& m = bot::macro;
            m.erase(
                std::remove_if(m.begin(), m.end(), [s](bot::Action const& a) { return a.step >= s; }),
                m.end()
            );
            // The macro prefix is still the source of truth for what the
            // player should be holding at this checkpoint.
            bot::syncPlayback(this);
        }
        else if (bot::mode == bot::Mode::Play) {
            bot::syncPlayback(this);
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        bot::setTimeScale(1.0f);
        bot::restoreAudioPitch();
        if (bot::mode == bot::Mode::Record) {
            bot::setMode(bot::Mode::Off);
            Notification::create("Recording stopped: level complete", NotificationIcon::Info)->show();
        }
    }
};

// ---------------------------------------------------------------------------
// Pause-menu UI
// ---------------------------------------------------------------------------
class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCNode* m_listContainer = nullptr;
        CCLabelBMFont* m_statusLabel = nullptr;
        CCLabelBMFont* m_fracLabel = nullptr;
        ButtonSprite* m_offSprite = nullptr;
        ButtonSprite* m_recSprite = nullptr;
        ButtonSprite* m_playSprite = nullptr;
        ButtonSprite* m_fxSprite = nullptr;
        TextInput* m_nameInput = nullptr;
        TextInput* m_speedInput = nullptr;
        bool m_refreshScheduled = false;
        float m_listW = 160.f;
        float m_listH = 100.f;
    };

    void updateStatusUI() {
        auto& f = m_fields;
        if (f->m_statusLabel) {
            std::string st = "OFF";
            if (bot::mode == bot::Mode::Record) st = fmt::format("REC  {} acts", bot::macro.size());
            else if (bot::mode == bot::Mode::Play) st = fmt::format("PLAY {} acts", bot::macro.size());
            else if (!bot::macro.empty()) st = fmt::format("OFF  {} acts loaded", bot::macro.size());
            f->m_statusLabel->setString(st.c_str());
        }
        // Calibration readout. `frac` is the sub-frame position we derived from
        // the OS event timestamp; if the two time bases ever disagreed it would
        // pin to the clamp, and `clamp n/m` makes that visible at a glance.
        // A healthy recording shows a spread like 0.03..0.97 with clamp 0/m.
        if (f->m_fracLabel) {
            std::string fs;
            if (bot::g_fracSamples == 0) {
                fs = "frac: no clicks yet";
            }
            else {
                fs = fmt::format("frac {:.4f}..{:.4f}  clamp {}/{}",
                                 bot::g_fracMin, bot::g_fracMax,
                                 bot::g_fracClamped, bot::g_fracSamples);
            }
            f->m_fracLabel->setString(fs.c_str());
        }
        if (f->m_offSprite)
            f->m_offSprite->setColor(bot::mode == bot::Mode::Off ? ccColor3B{ 80, 255, 80 } : ccColor3B{ 255, 255, 255 });
        if (f->m_recSprite)
            f->m_recSprite->setColor(bot::mode == bot::Mode::Record ? ccColor3B{ 255, 80, 80 } : ccColor3B{ 255, 255, 255 });
        if (f->m_playSprite)
            f->m_playSprite->setColor(bot::mode == bot::Mode::Play ? ccColor3B{ 80, 200, 255 } : ccColor3B{ 255, 255, 255 });
        if (f->m_fxSprite)
            f->m_fxSprite->setColor(bot::disableDeathEffects ? ccColor3B{ 255, 180, 80 } : ccColor3B{ 255, 255, 255 });
    }

    // ---- deferred list rebuild -------------------------------------------
    // We are inside a CCMenu activate callback when a row button fires, so the
    // list cannot be torn down synchronously. Bounce it through an action.
    void onDeferredRefresh() {
        m_fields->m_refreshScheduled = false;
        this->refreshMacroList();
    }

    void scheduleRefreshMacroList() {
        if (m_fields->m_refreshScheduled) return;
        m_fields->m_refreshScheduled = true;
        this->runAction(CCSequence::createWithTwoActions(
            CCDelayTime::create(0.0f),
            CCCallFunc::create(this, callfunc_selector(BotPauseLayer::onDeferredRefresh))
        ));
    }

    // ---- one row ----------------------------------------------------------
    static constexpr float kRowHeight = 26.f;

    CCNode* makeRow(std::filesystem::path path) {
        auto row = CCNode::create();
        row->setContentSize(CCSize{ m_fields->m_listW, kRowHeight });

        std::string name = path.stem().string();
        if (name.size() > 15) name = name.substr(0, 13) + "..";

        auto label = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
        label->setScale(0.4f);
        label->setAnchorPoint(CCPoint{ 0.f, 0.5f });
        label->setPosition(CCPoint{ 6.f, kRowHeight / 2.f });
        row->addChild(label);

        auto menu = CCMenu::create();
        menu->setContentSize(CCSize{ m_fields->m_listW, kRowHeight });
        menu->setPosition(CCPoint{ 0.f, 0.f });
        row->addChild(menu);

        // FIX: absolute == false, so the sprite sizes itself to the caption.
        // The original asked for a 16px-wide absolute box around "SAVE CURRENT".
        auto loadSpr = ButtonSprite::create("L", 0, false, "goldFont.fnt", "GJ_button_01.png", 0.f, 0.45f);
        auto loadBtn = CCMenuItemExt::createSpriteExtra(loadSpr, [this, path](CCMenuItemSpriteExtra*) {
            int lid = 0;
            if (!bot::loadMacro(path, bot::macro, lid)) {
                Notification::create("Failed to load macro (bad file?)", NotificationIcon::Error)->show();
                return;
            }
            bot::macroLevelID = lid;
            bot::setMode(bot::Mode::Off);
            this->updateStatusUI();

            int const cur = bot::currentLevelID();
            if (lid != 0 && cur != 0 && lid != cur) {
                Notification::create(
                    fmt::format("Macro is for level {}, this is {}", lid, cur),
                    NotificationIcon::Warning, 4.f
                )->show();
            }
            else {
                Notification::create("Macro loaded - press PLAY", NotificationIcon::Success)->show();
            }
        });
        loadBtn->setPosition(CCPoint{ m_fields->m_listW - 30.f, kRowHeight / 2.f });
        menu->addChild(loadBtn);

        auto delSpr = ButtonSprite::create("X", 0, false, "goldFont.fnt", "GJ_button_06.png", 0.f, 0.45f);
        auto delBtn = CCMenuItemExt::createSpriteExtra(delSpr, [this, path](CCMenuItemSpriteExtra*) {
            std::error_code ec;
            if (std::filesystem::remove(path, ec)) {
                Notification::create("Macro deleted", NotificationIcon::Info)->show();
            }
            else {
                Notification::create("Delete failed", NotificationIcon::Error)->show();
            }
            this->scheduleRefreshMacroList();
        });
        delBtn->setPosition(CCPoint{ m_fields->m_listW - 10.f, kRowHeight / 2.f });
        menu->addChild(delBtn);

        return row;
    }

    // ---- list -------------------------------------------------------------
    void refreshMacroList() {
        auto& f = m_fields;
        if (!f->m_listContainer) return;
        f->m_listContainer->removeAllChildren();

        std::vector<std::filesystem::path> files;
        std::error_code ec;
        auto dir = bot::getMacroDir();
        if (!dir.empty()) {
            std::filesystem::directory_iterator it(dir, ec);
            std::filesystem::directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                std::error_code fec;
                auto const& entry = *it;
                if (entry.is_regular_file(fec) && !fec && entry.path().extension() == ".gdbot") {
                    files.push_back(entry.path());
                }
            }
        }

        // Newest first: the generated names are timestamp-prefixed.
        std::sort(files.begin(), files.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
            return a.filename().string() > b.filename().string();
        });

        auto items = CCArray::create();
        for (size_t i = 0; i < files.size(); ++i) {
            items->addObject(this->makeRow(files[i]));
        }

        // FIX: the original used `ScrollLayer::create(CCSize)` together with a
        // raw CCContentLayer full of CCMenu/CCLabelBMFont children. The game's
        // CCScrollLayerExt has no bound static create() in 2.2081, and
        // CCContentLayer expects TableViewCell children -- geode::ListView with
        // GenericListCell is the supported way to build this.
        auto list = ListView::create(items, kRowHeight, f->m_listW, f->m_listH);
        if (!list) return;
        list->setPosition(CCPoint{ 0.f, 0.f });
        f->m_listContainer->addChild(list);
    }

    // ---- panel ------------------------------------------------------------
    void customSetup() {
        PauseLayer::customSetup();
        auto pl = PlayLayer::get();
        if (!pl) return;

        auto const winSize = CCDirector::sharedDirector()->getWinSize();
        float const panelW = 176.f;
        float const panelH = 292.f;
        float const panelX = winSize.width - panelW - 8.f;
        float const panelY = (winSize.height - panelH) / 2.f;

        auto panel = CCLayerColor::create(ccc4(0, 0, 0, 190), panelW, panelH);
        panel->setPosition(CCPoint{ panelX, panelY });
        this->addChild(panel, 100);

        auto title = CCLabelBMFont::create("CBF TAS BOT", "goldFont.fnt");
        title->setScale(0.5f);
        title->setPosition(CCPoint{ panelW / 2.f, panelH - 16.f });
        panel->addChild(title);

        m_fields->m_statusLabel = CCLabelBMFont::create("OFF", "chatFont.fnt");
        m_fields->m_statusLabel->setScale(0.5f);
        m_fields->m_statusLabel->setPosition(CCPoint{ panelW / 2.f, panelH - 34.f });
        panel->addChild(m_fields->m_statusLabel);

        m_fields->m_fracLabel = CCLabelBMFont::create("frac: no clicks yet", "chatFont.fnt");
        m_fields->m_fracLabel->setScale(0.38f);
        m_fields->m_fracLabel->setPosition(CCPoint{ panelW / 2.f, panelH - 48.f });
        panel->addChild(m_fields->m_fracLabel);

        auto modeMenu = CCMenu::create();
        modeMenu->setContentSize(CCSize{ panelW, 24.f });
        modeMenu->setPosition(CCPoint{ 0.f, panelH - 70.f });
        panel->addChild(modeMenu);

        m_fields->m_offSprite = ButtonSprite::create("OFF", 0, false, "goldFont.fnt", "GJ_button_01.png", 0.f, 0.45f);
        auto offBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_offSprite, [this](CCMenuItemSpriteExtra*) {
            bot::setMode(bot::Mode::Off);
            this->updateStatusUI();
        });
        offBtn->setPosition(CCPoint{ panelW * 0.2f, 12.f });
        modeMenu->addChild(offBtn);

        m_fields->m_recSprite = ButtonSprite::create("REC", 0, false, "goldFont.fnt", "GJ_button_01.png", 0.f, 0.45f);
        auto recBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_recSprite, [this](CCMenuItemSpriteExtra*) {
            bot::macro.clear();
            bot::macroLevelID = bot::currentLevelID();
            bot::setMode(bot::Mode::Record);
            this->updateStatusUI();
            Notification::create("Recording started", NotificationIcon::Info)->show();
            if (auto p = PlayLayer::get()) p->resetLevel();
        });
        recBtn->setPosition(CCPoint{ panelW * 0.5f, 12.f });
        modeMenu->addChild(recBtn);

        m_fields->m_playSprite = ButtonSprite::create("PLAY", 0, false, "goldFont.fnt", "GJ_button_01.png", 0.f, 0.45f);
        auto playBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_playSprite, [this](CCMenuItemSpriteExtra*) {
            if (bot::macro.empty()) {
                Notification::create("No macro loaded", NotificationIcon::Warning)->show();
                return;
            }
            int const cur = bot::currentLevelID();
            if (bot::macroLevelID != 0 && cur != 0 && bot::macroLevelID != cur) {
                Notification::create(
                    fmt::format("Macro is for level {}, refusing to play on {}", bot::macroLevelID, cur),
                    NotificationIcon::Error, 4.f
                )->show();
                return;
            }
            bot::setMode(bot::Mode::Play);
            this->updateStatusUI();
            Notification::create("Playing...", NotificationIcon::Success)->show();
            if (auto p = PlayLayer::get()) p->resetLevel();
        });
        playBtn->setPosition(CCPoint{ panelW * 0.8f, 12.f });
        modeMenu->addChild(playBtn);

        // Speed row
        auto speedLbl = CCLabelBMFont::create("Speed:", "chatFont.fnt");
        speedLbl->setScale(0.45f);
        speedLbl->setAnchorPoint(CCPoint{ 0.f, 0.5f });
        speedLbl->setPosition(CCPoint{ 10.f, panelH - 96.f });
        panel->addChild(speedLbl);

        m_fields->m_speedInput = TextInput::create(60.f, "1.00", "chatFont.fnt");
        m_fields->m_speedInput->setCommonFilter(CommonFilter::Float);
        m_fields->m_speedInput->setMaxCharCount(6);
        m_fields->m_speedInput->setScale(0.7f);
        m_fields->m_speedInput->setPosition(CCPoint{ 86.f, panelH - 96.f });
        m_fields->m_speedInput->setString(fmt::format("{:.2f}", bot::recSpeed));
        m_fields->m_speedInput->setCallback([](std::string const& str) {
            bot::recSpeed = bot::parseSpeedString(str);
            if (auto mod = Mod::get()) mod->setSavedValue<double>("recSpeed", bot::recSpeed);
        });
        panel->addChild(m_fields->m_speedInput);

        auto fxMenu = CCMenu::create();
        fxMenu->setContentSize(CCSize{ 40.f, 24.f });
        fxMenu->setPosition(CCPoint{ panelW - 44.f, panelH - 108.f });
        panel->addChild(fxMenu);

        m_fields->m_fxSprite = ButtonSprite::create("FX", 0, false, "goldFont.fnt", "GJ_button_01.png", 0.f, 0.45f);
        auto fxBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_fxSprite, [this](CCMenuItemSpriteExtra*) {
            bot::disableDeathEffects = !bot::disableDeathEffects;
            if (auto mod = Mod::get()) mod->setSavedValue<bool>("disableDeathEffects", bot::disableDeathEffects);
            this->updateStatusUI();
            Notification::create(
                bot::disableDeathEffects ? "Death / wave / respawn FX: OFF" : "Death / wave / respawn FX: ON",
                NotificationIcon::Info
            )->show();
        });
        fxBtn->setPosition(CCPoint{ 20.f, 12.f });
        fxMenu->addChild(fxBtn);

        // Name row
        auto nameLbl = CCLabelBMFont::create("Name:", "chatFont.fnt");
        nameLbl->setScale(0.45f);
        nameLbl->setAnchorPoint(CCPoint{ 0.f, 0.5f });
        nameLbl->setPosition(CCPoint{ 10.f, panelH - 122.f });
        panel->addChild(nameLbl);

        m_fields->m_nameInput = TextInput::create(104.f, bot::generateTimestampName().c_str(), "chatFont.fnt");
        m_fields->m_nameInput->setMaxCharCount(48);
        m_fields->m_nameInput->setScale(0.7f);
        m_fields->m_nameInput->setPosition(CCPoint{ 100.f, panelH - 122.f });
        panel->addChild(m_fields->m_nameInput);

        auto saveMenu = CCMenu::create();
        saveMenu->setContentSize(CCSize{ panelW - 16.f, 24.f });
        saveMenu->setPosition(CCPoint{ 8.f, panelH - 160.f });
        panel->addChild(saveMenu);

        auto saveSpr = ButtonSprite::create("SAVE CURRENT", 0, false, "goldFont.fnt", "GJ_button_01.png", 0.f, 0.45f);
        auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](CCMenuItemSpriteExtra*) {
            if (bot::macro.empty()) {
                Notification::create("Macro is empty", NotificationIcon::Warning)->show();
                return;
            }
            std::string const raw = m_fields->m_nameInput
                ? std::string(m_fields->m_nameInput->getString().c_str())
                : std::string();
            std::string const clean = bot::sanitizeFileName(raw);
            std::string const stem = clean.empty() ? bot::generateTimestampName() : clean;

            auto dir = bot::getMacroDir();
            if (dir.empty()) {
                Notification::create("Save failed: no config dir", NotificationIcon::Error)->show();
                return;
            }
            auto path = dir / (stem + ".gdbot");
            std::error_code ec;
            bool const existed = std::filesystem::exists(path, ec);

            int const lid = bot::macroLevelID != 0 ? bot::macroLevelID : bot::currentLevelID();
            if (bot::saveMacro(path, bot::macro, lid)) {
                bot::macroLevelID = lid;
                this->scheduleRefreshMacroList();
                Notification::create(existed ? "Macro overwritten" : "Macro saved", NotificationIcon::Success)->show();
            }
            else {
                Notification::create("Save failed", NotificationIcon::Error)->show();
            }
        });
        saveBtn->setPosition(CCPoint{ (panelW - 16.f) / 2.f, 12.f });
        saveMenu->addChild(saveBtn);

        m_fields->m_listW = panelW - 12.f;
        m_fields->m_listH = 110.f;
        m_fields->m_listContainer = CCNode::create();
        m_fields->m_listContainer->setContentSize(CCSize{ m_fields->m_listW, m_fields->m_listH });
        m_fields->m_listContainer->setAnchorPoint(CCPoint{ 0.f, 0.f });
        m_fields->m_listContainer->setPosition(CCPoint{ 6.f, 10.f });
        panel->addChild(m_fields->m_listContainer);

        this->updateStatusUI();
        this->refreshMacroList();
    }
};
