// CBF-style TAS bot for Geometry Dash 2.2081 / Geode 5.x (Android & PC)
// Полная поддержка 100k+ TPS саб-тиков на любых скоростях (включая 0.01x), непрерывная шкала времени, сохранение иконок и чекпоинты.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/GJGarageLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <fmod.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Безопасное сохранение и восстановление третьего цвета и свечения через C++20 concepts
// ---------------------------------------------------------------------------
template <typename GM>
inline void saveColor3IfAvailable(GM* gm, Mod* mod) {
    if constexpr (requires { gm->m_playerColor3; }) {
        if (gm->m_playerColor3 >= 0) mod->setSavedValue("saved_color3", static_cast<int>(gm->m_playerColor3));
    }
    else if constexpr (requires { gm->m_playerGlowColor; }) {
        if (gm->m_playerGlowColor >= 0) mod->setSavedValue("saved_color3", static_cast<int>(gm->m_playerGlowColor));
    }
    else if constexpr (requires { gm->m_playerColorGlow; }) {
        if (gm->m_playerColorGlow >= 0) mod->setSavedValue("saved_color3", static_cast<int>(gm->m_playerColorGlow));
    }
}

template <typename GM>
inline void restoreColor3IfAvailable(GM* gm, int col3) {
    if (col3 < 0) return;
    if constexpr (requires { gm->m_playerColor3 = col3; }) {
        gm->m_playerColor3 = col3;
    }
    else if constexpr (requires { gm->m_playerGlowColor = col3; }) {
        gm->m_playerGlowColor = col3;
    }
    else if constexpr (requires { gm->m_playerColorGlow = col3; }) {
        gm->m_playerColorGlow = col3;
    }
}

template <typename GM>
inline void saveGlowIfAvailable(GM* gm, Mod* mod) {
    if constexpr (requires { gm->m_playerGlow; }) {
        mod->setSavedValue("saved_glow", static_cast<bool>(gm->m_playerGlow));
    }
}

template <typename GM>
inline void restoreGlowIfAvailable(GM* gm, Mod* mod) {
    if constexpr (requires { gm->m_playerGlow; }) {
        if (mod->hasSavedValue("saved_glow")) {
            gm->m_playerGlow = mod->getSavedValue<bool>("saved_glow");
        }
    }
}

// ---------------------------------------------------------------------------
// Сохранение и восстановление иконок/цветов (обход сброса GD 2.2 verifyLoadedDatabase)
// ---------------------------------------------------------------------------
inline void saveCustomIcons() {
    auto gm = GameManager::sharedState();
    if (!gm) return;
    auto mod = Mod::get();
    if (gm->m_playerFrame > 0) mod->setSavedValue("saved_frame", static_cast<int>(gm->m_playerFrame));
    if (gm->m_playerShip > 0) mod->setSavedValue("saved_ship", static_cast<int>(gm->m_playerShip));
    if (gm->m_playerBall > 0) mod->setSavedValue("saved_ball", static_cast<int>(gm->m_playerBall));
    if (gm->m_playerBird > 0) mod->setSavedValue("saved_bird", static_cast<int>(gm->m_playerBird));
    if (gm->m_playerDart > 0) mod->setSavedValue("saved_dart", static_cast<int>(gm->m_playerDart));
    if (gm->m_playerRobot > 0) mod->setSavedValue("saved_robot", static_cast<int>(gm->m_playerRobot));
    if (gm->m_playerSpider > 0) mod->setSavedValue("saved_spider", static_cast<int>(gm->m_playerSpider));
    if (gm->m_playerSwing > 0) mod->setSavedValue("saved_swing", static_cast<int>(gm->m_playerSwing));
    if (gm->m_playerJetpack > 0) mod->setSavedValue("saved_jetpack", static_cast<int>(gm->m_playerJetpack));
    if (gm->m_playerDeathEffect > 0) mod->setSavedValue("saved_deathEffect", static_cast<int>(gm->m_playerDeathEffect));
    if (gm->m_playerStreak > 0) mod->setSavedValue("saved_streak", static_cast<int>(gm->m_playerStreak));
    if (gm->m_playerColor >= 0) mod->setSavedValue("saved_color1", static_cast<int>(gm->m_playerColor));
    if (gm->m_playerColor2 >= 0) mod->setSavedValue("saved_color2", static_cast<int>(gm->m_playerColor2));
    saveColor3IfAvailable(gm, mod);
    saveGlowIfAvailable(gm, mod);
}

inline void restoreCustomIcons() {
    auto gm = GameManager::sharedState();
    if (!gm) return;
    auto mod = Mod::get();

    int frame = mod->getSavedValue<int>("saved_frame", -1);
    if (frame > 0) gm->m_playerFrame = frame;

    int ship = mod->getSavedValue<int>("saved_ship", -1);
    if (ship > 0) gm->m_playerShip = ship;

    int ball = mod->getSavedValue<int>("saved_ball", -1);
    if (ball > 0) gm->m_playerBall = ball;

    int bird = mod->getSavedValue<int>("saved_bird", -1);
    if (bird > 0) gm->m_playerBird = bird;

    int dart = mod->getSavedValue<int>("saved_dart", -1);
    if (dart > 0) gm->m_playerDart = dart;

    int robot = mod->getSavedValue<int>("saved_robot", -1);
    if (robot > 0) gm->m_playerRobot = robot;

    int spider = mod->getSavedValue<int>("saved_spider", -1);
    if (spider > 0) gm->m_playerSpider = spider;

    int swing = mod->getSavedValue<int>("saved_swing", -1);
    if (swing > 0) gm->m_playerSwing = swing;

    int jetpack = mod->getSavedValue<int>("saved_jetpack", -1);
    if (jetpack > 0) gm->m_playerJetpack = jetpack;

    int death = mod->getSavedValue<int>("saved_deathEffect", -1);
    if (death > 0) gm->m_playerDeathEffect = death;

    int streak = mod->getSavedValue<int>("saved_streak", -1);
    if (streak > 0) gm->m_playerStreak = streak;

    int col1 = mod->getSavedValue<int>("saved_color1", -1);
    if (col1 >= 0) gm->m_playerColor = col1;

    int col2 = mod->getSavedValue<int>("saved_color2", -1);
    if (col2 >= 0) gm->m_playerColor2 = col2;

    int col3 = mod->getSavedValue<int>("saved_color3", -1);
    restoreColor3IfAvailable(gm, col3);
    restoreGlowIfAvailable(gm, mod);
}

// ---------------------------------------------------------------------------
// Разблокировка всех предметов и сохранение
// ---------------------------------------------------------------------------
class $modify(BotGameManager, GameManager) {
    bool isIconUnlocked(int id, IconType type) {
        return true;
    }

    bool isColorUnlocked(int id, UnlockType type) {
        return true;
    }

    void dataLoaded(DS_Dictionary* dict) {
        GameManager::dataLoaded(dict);
        restoreCustomIcons();
    }

    void firstLoad() {
        GameManager::firstLoad();
        restoreCustomIcons();
    }

    void save() {
        saveCustomIcons();
        GameManager::save();
    }
};

class $modify(BotGameStatsManager, GameStatsManager) {
    bool isItemUnlocked(UnlockType type, int id) {
        return true;
    }
};

class $modify(BotGarageLayer, GJGarageLayer) {
    bool init() {
        restoreCustomIcons();
        return GJGarageLayer::init();
    }

    void onSelectTab(cocos2d::CCObject* sender) {
        saveCustomIcons();
        GameManager::sharedState()->save();
        GJGarageLayer::onSelectTab(sender);
    }

    void onBack(cocos2d::CCObject* sender) {
        saveCustomIcons();
        GameManager::sharedState()->save();
        GJGarageLayer::onBack(sender);
    }

    ~BotGarageLayer() {
        saveCustomIcons();
        GameManager::sharedState()->save();
    }
};

class $modify(BotMenuLayer, MenuLayer) {
    bool init() {
        restoreCustomIcons();
        return MenuLayer::init();
    }
};

namespace bot {

    constexpr uint32_t kMagic = 0x32544F42; // "BOT2"

    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t step = 0;   // номер физического шага от старта попытки
        double frac = 0.0;   // субтиковая доля внутри шага [0..1)
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
        double timestamp = 0.0;
    };

    // ---- состояние бота ----
    inline Mode mode = Mode::Off;
    inline std::vector<Action> macro;
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

    inline bool disableDeathEffects = Mod::get()->getSavedValue<bool>("disableDeathEffects", true);

    // ---- непрерывная субтиковая шкала CBF ----
    inline std::vector<RawInput> pendingRecordInputs;
    inline double g_stepAnchorWallTime = 0.0;
    inline uint32_t g_stepAnchorStep = 0;

    inline bool inputThisStep = false;
    inline bool p1Split = false;
    inline bool p2Split = false;
    inline bool midStep = false;
    inline CCPoint p1Pos = { 0.f, 0.f };
    inline CCPoint p2Pos = { 0.f, 0.f };
    inline float rotationDelta = 0.f;

    inline double getCurrentTimestamp() {
        auto const now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration<double>(now).count();
    }

    inline bool isPlayLayer(GJBaseGameLayer* l) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == l;
    }

    inline void restoreAudioPitch() {
        auto fae = FMODAudioEngine::sharedEngine();
        if (!fae || !fae->m_backgroundMusicChannel) return;
        fae->m_backgroundMusicChannel->setPitch(1.0f);
    }

    inline void updateAudioPitch() {
        auto fae = FMODAudioEngine::sharedEngine();
        if (!fae || !fae->m_backgroundMusicChannel) return;

        auto pl = PlayLayer::get();
        bool shouldSlow = (mode == Mode::Record) && pl && !pl->m_playerDied && (appliedScale > 0.001f);
        float target = shouldSlow ? appliedScale : 1.0f;
        target = std::clamp(target, 0.01f, 2.0f);

        float cur = 1.0f;
        fae->m_backgroundMusicChannel->getPitch(&cur);
        if (std::abs(cur - target) > 1e-4f) {
            fae->m_backgroundMusicChannel->setPitch(target);
        }
    }

    inline void resetEngine() {
        pendingRecordInputs.clear();
        g_stepAnchorWallTime = getCurrentTimestamp();
        g_stepAnchorStep = step;
        inputThisStep = false;
        p1Split = false;
        p2Split = false;
        midStep = false;
    }

    inline void applyClickFlags() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (mode == Mode::Record) {
            pl->m_clickBetweenSteps = true;
            pl->m_clickOnSteps = false;
        }
        else if (mode == Mode::Play) {
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
        g_stepAnchorWallTime = getCurrentTimestamp();
        g_stepAnchorStep = step;
        appliedScale = s;
        CCDirector::sharedDirector()->getScheduler()->setTimeScale(s);
        updateAudioPitch();
    }

    inline void setMode(Mode m) {
        mode = m;
        applyClickFlags();
        resetEngine();
        if (mode == Mode::Off) {
            setTimeScale(1.0f);
            restoreAudioPitch();
        } else {
            updateAudioPitch();
        }
    }

    inline void recordRawInput(int button, bool push, bool isPlayer1, double timestamp) {
        RawInput raw;
        raw.button = button;
        raw.push = push;
        raw.p1 = isPlayer1;
        raw.timestamp = timestamp;
        pendingRecordInputs.push_back(raw);
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

    inline std::filesystem::path getMacroDir() {
        auto dir = Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
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
        if (out.size() > 80) out.resize(80);
        return out;
    }

    inline bool saveMacro(std::filesystem::path const& path, std::vector<Action> const& actions) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        uint32_t magic = kMagic;
        uint32_t count = static_cast<uint32_t>(actions.size());
        out.write(reinterpret_cast<char const*>(&magic), 4);
        out.write(reinterpret_cast<char const*>(&count), 4);
        for (auto const& a : actions) {
            uint8_t flags = static_cast<uint8_t>((a.down ? 1 : 0) | (a.p1 ? 2 : 0));
            out.write(reinterpret_cast<char const*>(&a.step), 4);
            out.write(reinterpret_cast<char const*>(&a.frac), 8);
            out.write(reinterpret_cast<char const*>(&flags), 1);
            out.write(reinterpret_cast<char const*>(&a.button), 1);
        }
        return static_cast<bool>(out);
    }

    inline bool loadMacro(std::filesystem::path const& path, std::vector<Action>& actions) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        uint32_t magic = 0, count = 0;
        in.read(reinterpret_cast<char*>(&magic), 4);
        in.read(reinterpret_cast<char*>(&count), 4);
        if (!in || magic != kMagic || count > 5'000'000) return false;
        std::vector<Action> tmp;
        tmp.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Action a;
            uint8_t flags = 0;
            in.read(reinterpret_cast<char*>(&a.step), 4);
            in.read(reinterpret_cast<char*>(&a.frac), 8);
            in.read(reinterpret_cast<char*>(&flags), 1);
            in.read(reinterpret_cast<char*>(&a.button), 1);
            if (!in) return false;
            a.down = (flags & 1) != 0;
            a.p1 = (flags & 2) != 0;
            tmp.push_back(a);
        }
        actions = std::move(tmp);
        return true;
    }

    inline void applyInput(PlayLayer* pl, StepSub const& s, uint32_t st, double frac) {
        ++applying;
        applyStep = st;
        applyFrac = std::clamp(frac, 0.0, 1.0);
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

        // Убираем ложный буфер прыжка куба/робота/паука при загрузке чекпоинта в воздухе
        if (pl->m_player1 && !pl->m_player1->m_isOnGround &&
            !pl->m_player1->m_isShip && !pl->m_player1->m_isBird && !pl->m_player1->m_isDart && !pl->m_player1->m_isSwing) {
            pl->m_player1->m_jumpBuffered = false;
        }
        if (pl->m_player2 && !pl->m_player2->m_isOnGround &&
            !pl->m_player2->m_isShip && !pl->m_player2->m_isBird && !pl->m_player2->m_isDart && !pl->m_player2->m_isSwing) {
            pl->m_player2->m_jumpBuffered = false;
        }
    }

    inline void onFrameStart() {
        PlayLayer* pl = PlayLayer::get();
        CCNode* par = nullptr;
        bool idle = !pl
            || !(par = pl->getParent())
            || par->getChildByType<PauseLayer>(0)
            || pl->getChildByType<EndLevelLayer>(0);

        setTimeScale((mode == Mode::Record && !idle) ? static_cast<float>(recSpeed) : 1.0f);

        if (mode == Mode::Off) return;

        if (idle) {
            pendingRecordInputs.clear();
            g_stepAnchorWallTime = getCurrentTimestamp();
            g_stepAnchorStep = step;
        }
    }

    inline void clearCollisionLog(PlayerObject* p) {
        if (!p) return;
        p->m_collisionLogTop->removeAllObjects();
        p->m_collisionLogBottom->removeAllObjects();
        p->m_collisionLogLeft->removeAllObjects();
        p->m_collisionLogRight->removeAllObjects();
        p->m_lastCollisionLeft = -1;
        p->m_lastCollisionRight = -1;
        p->m_lastCollisionBottom = -1;
        p->m_lastCollisionTop = -1;
    }
}

// ---------------------------------------------------------------------------
// Глушение ударной волны смерти
// ---------------------------------------------------------------------------
class $modify(BotCircleWave, CCCircleWave) {
    void update(float dt) {
        if (bot::disableDeathEffects && bot::mode != bot::Mode::Off) {
            this->removeFromParentAndCleanup(true);
            return;
        }
        CCCircleWave::update(dt);
    }
};

// ---------------------------------------------------------------------------
// Контроллер времени кадра и аудио-синхронизации
// ---------------------------------------------------------------------------
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        bot::onFrameStart();
        CCScheduler::update(dt);
        bot::updateAudioPitch();
    }
};

// ---------------------------------------------------------------------------
// Прямой высокоточный перехват аппаратного ввода (Windows / Android)
// ---------------------------------------------------------------------------
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void pushButton(int button, bool isPlayer2) {
        if (bot::mode == bot::Mode::Record && bot::isPlayLayer(this)) {
            double const now = bot::getCurrentTimestamp();
            bot::recordRawInput(button, true, !isPlayer2, now);
            return;
        }
        if (bot::mode == bot::Mode::Play && bot::isPlayLayer(this)) {
            return;
        }
        GJBaseGameLayer::pushButton(button, isPlayer2);
    }

    void releaseButton(int button, bool isPlayer2) {
        if (bot::mode == bot::Mode::Record && bot::isPlayLayer(this)) {
            double const now = bot::getCurrentTimestamp();
            bot::recordRawInput(button, false, !isPlayer2, now);
            return;
        }
        if (bot::mode == bot::Mode::Play && bot::isPlayLayer(this)) {
            return;
        }
        GJBaseGameLayer::releaseButton(button, isPlayer2);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        using namespace bot;
        if (mode == Mode::Off || silent > 0 || !isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (mode == Mode::Play) {
            if (applying > 0) GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (applying > 0) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (bot::mode != bot::Mode::Off && bot::isPlayLayer(this)) {
            this->m_queuedButtons.clear();
            return;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------------------
// Потоковая подтиковая нарезка шагов с мульти-сплитом (100k+ TPS)
// ---------------------------------------------------------------------------
class $modify(BotPlayerObject, PlayerObject) {
    void playSpawnEffect() {
        this->setVisible(true);
        if (bot::disableDeathEffects && bot::mode != bot::Mode::Off) {
            return;
        }
        PlayerObject::playSpawnEffect();
    }

    void playDeathEffect() {
        if (bot::disableDeathEffects && bot::mode != bot::Mode::Off) {
            return;
        }
        PlayerObject::playDeathEffect();
    }

    void update(float stepDelta) {
        using namespace bot;
        auto pl = PlayLayer::get();
        if (!pl || midStep) {
            PlayerObject::update(stepDelta);
            return;
        }

        if (stepDelta <= 1e-9f) {
            PlayerObject::update(stepDelta);
            return;
        }

        if (mode == Mode::Off) {
            PlayerObject::update(stepDelta);
            if (this == pl->m_player1) ++step;
            return;
        }

        if (this != pl->m_player1) {
            if (!inputThisStep || this != pl->m_player2) {
                PlayerObject::update(stepDelta);
            }
            return;
        }

        uint32_t const cur = step;
        std::vector<StepSub> subs;

        float const scale = (appliedScale > 0.0001f) ? appliedScale : 1.0f;
        double const stepDuration = static_cast<double>(stepDelta) / static_cast<double>(scale);
        double const now = getCurrentTimestamp();

        // Защита от фазового дрейфа шкалы при смене FPS или лагах
        if (g_stepAnchorWallTime <= 0.0 || std::abs(now - g_stepAnchorWallTime) > 3.0) {
            g_stepAnchorWallTime = now;
            g_stepAnchorStep = cur;
        }

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
            for (auto const& in : pendingRecordInputs) {
                double stepsOffset = (in.timestamp - g_stepAnchorWallTime) / stepDuration;
                int64_t targetStep = static_cast<int64_t>(g_stepAnchorStep) + static_cast<int64_t>(std::floor(stepsOffset));
                double frac = stepsOffset - std::floor(stepsOffset);

                if (targetStep <= static_cast<int64_t>(cur)) {
                    StepSub s;
                    s.button = in.button;
                    s.push = in.push;
                    s.p1 = in.p1;
                    if (targetStep < static_cast<int64_t>(cur)) {
                        s.frac = 0.0001;
                    } else {
                        s.frac = std::clamp(frac, 0.0001, 0.9999);
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
            pendingRecordInputs = std::move(remaining);
        }

        inputThisStep = !subs.empty();

        if (subs.empty()) {
            p1Split = false;
            p2Split = false;
            PlayerObject::update(stepDelta);
            g_stepAnchorWallTime += stepDuration;
            g_stepAnchorStep = cur + 1;
            ++step;
            return;
        }

        // Сортировка действий внутри шага по субтиковому времени
        std::stable_sort(subs.begin(), subs.end(), [](auto const& a, auto const& b) {
            return a.frac < b.frac;
        });

        // Гарантируем строго положительную дельту между кликами внутри одного тика (100k+ TPS)
        for (size_t i = 1; i < subs.size(); ++i) {
            if (subs[i].frac <= subs[i - 1].frac + 1e-5) {
                subs[i].frac = std::min(0.9999, subs[i - 1].frac + 1e-5);
            }
        }

        PlayerObject* p2 = pl->m_player2;
        bool isDual = pl->m_gameState.m_isDualMode;
        bool p1StartedOnGround = this->m_isOnGround;
        bool p2StartedOnGround = p2 ? p2->m_isOnGround : false;

        p1Pos = PlayerObject::getPosition();
        p2Pos = p2 ? p2->getPosition() : CCPoint{ 0.f, 0.f };

        p1Split = true;
        p2Split = isDual && (p2 != nullptr);

        midStep = true;
        double lastFrac = 0.0;

        for (size_t i = 0; i < subs.size(); ++i) {
            auto const& s = subs[i];
            double subFactor = std::max(0.0, s.frac - lastFrac);
            float substepDelta = static_cast<float>(stepDelta * subFactor);
            rotationDelta = substepDelta;

            if (substepDelta > 1e-9f) {
                PlayerObject::update(substepDelta);
                if (i == 0 && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) this->m_isOnGround = p1StartedOnGround;
                if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true);
                else pl->checkCollisions(this, stepDelta, true);
                PlayerObject::updateRotation(substepDelta);
                bot::clearCollisionLog(this);

                if (p2Split && p2) {
                    p2->update(substepDelta);
                    if (i == 0 && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) p2->m_isOnGround = p2StartedOnGround;
                    if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
                    else pl->checkCollisions(p2, stepDelta, true);
                    p2->updateRotation(substepDelta);
                    bot::clearCollisionLog(p2);
                }
            }

            applyInput(pl, s, cur, s.frac);
            lastFrac = s.frac;
        }

        // Оставшаяся часть шага после последнего саб-клика
        double remainingFactor = std::max(0.0, 1.0 - lastFrac);
        float remainingDelta = static_cast<float>(stepDelta * remainingFactor);
        rotationDelta = remainingDelta;

        if (remainingDelta > 1e-9f) {
            PlayerObject::update(remainingDelta);
            if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true);
            else pl->checkCollisions(this, stepDelta, true);
            PlayerObject::updateRotation(remainingDelta);
            bot::clearCollisionLog(this);

            if (p2Split && p2) {
                p2->update(remainingDelta);
                if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
                else pl->checkCollisions(p2, stepDelta, true);
                p2->updateRotation(remainingDelta);
                bot::clearCollisionLog(p2);
            }
        }

        midStep = false;
        g_stepAnchorWallTime += stepDuration;
        g_stepAnchorStep = cur + 1;
        ++step;
    }

    void updateRotation(float t) {
        auto pl = PlayLayer::get();
        if (pl && bot::mode != bot::Mode::Off && !bot::midStep) {
            if (this == pl->m_player1 && bot::p1Split) {
                PlayerObject::updateRotation(bot::rotationDelta);
                if (!this->m_isDart) this->m_lastPosition = bot::p1Pos;
                return;
            }
            if (this == pl->m_player2 && bot::p2Split) {
                PlayerObject::updateRotation(bot::rotationDelta);
                if (!this->m_isDart) this->m_lastPosition = bot::p2Pos;
                return;
            }
        }
        PlayerObject::updateRotation(t);
    }
};

// ---------------------------------------------------------------------------
// Уровень, чекпоинты, рестарты, эффекты смерти
// ---------------------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        restoreCustomIcons();

        bot::mode = bot::Mode::Off;
        bot::step = 0;
        bot::playIndex = 0;
        bot::checkpoints.clear();
        bot::resetEngine();
        bot::restoreAudioPitch();

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        bot::applyClickFlags();

        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            Notification::create("Disable Click Between Frames mod: it conflicts with the bot", NotificationIcon::Warning, 4.f)->show();
        }
        return true;
    }

    void onQuit() {
        bot::setTimeScale(1.0f);
        bot::restoreAudioPitch();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        bot::cpLoaded = false;
        PlayLayer::resetLevel();
        bot::resetEngine();
        bot::updateAudioPitch();

        if (this->m_player1) this->m_player1->setVisible(true);
        if (this->m_player2) this->m_player2->setVisible(true);

        if (bot::cpLoaded) return;

        bot::step = 0;
        bot::playIndex = 0;
        bot::checkpoints.clear();
        if (bot::mode == bot::Mode::Record) bot::macro.clear();
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
                Notification::create("Bot: unknown checkpoint, OFF", NotificationIcon::Warning)->show();
            }
            return;
        }

        bot::step = it->second;
        bot::g_stepAnchorStep = bot::step;
        bot::g_stepAnchorWallTime = bot::getCurrentTimestamp();
        bot::pendingRecordInputs.clear();

        if (bot::mode == bot::Mode::Record) {
            uint32_t s = bot::step;
            auto& m = bot::macro;
            m.erase(
                std::remove_if(m.begin(), m.end(), [s](bot::Action const& a) { return a.step >= s; }),
                m.end()
            );
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

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (!bot::disableDeathEffects || bot::mode == bot::Mode::Off) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }

        auto myPos = this->getPosition();
        PlayLayer::destroyPlayer(player, object);
        this->stopAllActions();
        this->setPosition(myPos);
    }
};

// ---------------------------------------------------------------------------
// Интерфейс меню паузы
// ---------------------------------------------------------------------------
class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCNode* m_fileListContainer = nullptr;
        CCLabelBMFont* m_statusLabel = nullptr;
        ButtonSprite* m_offSprite = nullptr;
        ButtonSprite* m_recSprite = nullptr;
        ButtonSprite* m_playSprite = nullptr;
        ButtonSprite* m_fxSprite = nullptr;
        TextInput* m_nameInput = nullptr;
    };

    void updateStatusUI() {
        if (m_fields->m_statusLabel) {
            std::string st = "OFF";
            if (bot::mode == bot::Mode::Record) st = fmt::format("REC ({} acts)", bot::macro.size());
            else if (bot::mode == bot::Mode::Play) st = fmt::format("PLAY ({} acts)", bot::macro.size());
            else if (!bot::macro.empty()) st = fmt::format("OFF ({} acts loaded)", bot::macro.size());
            m_fields->m_statusLabel->setString(st.c_str());
        }
        if (m_fields->m_offSprite) {
            m_fields->m_offSprite->setColor(bot::mode == bot::Mode::Off ? ccColor3B{ 80, 255, 80 } : ccColor3B{ 255, 255, 255 });
        }
        if (m_fields->m_recSprite) {
            m_fields->m_recSprite->setColor(bot::mode == bot::Mode::Record ? ccColor3B{ 255, 80, 80 } : ccColor3B{ 255, 255, 255 });
        }
        if (m_fields->m_playSprite) {
            m_fields->m_playSprite->setColor(bot::mode == bot::Mode::Play ? ccColor3B{ 80, 200, 255 } : ccColor3B{ 255, 255, 255 });
        }
        if (m_fields->m_fxSprite) {
            m_fields->m_fxSprite->setColor(bot::disableDeathEffects ? ccColor3B{ 255, 180, 80 } : ccColor3B{ 255, 255, 255 });
        }
    }

    void refreshMacroList() {
        if (!m_fields->m_fileListContainer) return;
        m_fields->m_fileListContainer->removeAllChildren();

        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (auto const& entry : std::filesystem::directory_iterator(bot::getMacroDir(), ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".gdbot") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end(), [](auto const& a, auto const& b) {
            return a.filename().string() > b.filename().string();
        });

        float viewW = 160.0f;
        float viewH = 100.0f;
        float rowH = 22.0f;
        float totalH = std::max(viewH, static_cast<float>(files.size()) * rowH);

        auto scroll = ScrollLayer::create(CCSize{ viewW, viewH });
        scroll->m_contentLayer->setContentSize(CCSize{ viewW, totalH });

        for (size_t i = 0; i < files.size(); ++i) {
            auto path = files[i];
            float y = totalH - static_cast<float>(i + 1) * rowH;

            auto rowNode = CCNode::create();
            rowNode->setPosition(0.0f, y);
            rowNode->setContentSize(CCSize{ viewW, rowH });

            std::string name = path.stem().string();
            if (name.length() > 14) name = name.substr(0, 12) + "..";
            auto lbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
            lbl->setScale(0.4f);
            lbl->setAnchorPoint(CCPoint{ 0.0f, 0.5f });
            lbl->setPosition(4.0f, rowH / 2.0f);
            rowNode->addChild(lbl);

            auto rowMenu = CCMenu::create();
            rowMenu->setPosition(0.0f, 0.0f);

            auto loadSpr = ButtonSprite::create("L", 12, true, "goldFont.fnt", "GJ_button_01.png", 16.0f, 0.4f);
            auto loadBtn = CCMenuItemExt::createSpriteExtra(loadSpr, [this, path](CCObject*) {
                if (bot::loadMacro(path, bot::macro)) {
                    bot::setMode(bot::Mode::Off);
                    this->updateStatusUI();
                    Notification::create("Macro loaded. Press PLAY", NotificationIcon::Success)->show();
                }
                else {
                    Notification::create("Failed to load", NotificationIcon::Error)->show();
                }
            });
            loadBtn->setPosition(120.0f, rowH / 2.0f);
            rowMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("X", 12, true, "goldFont.fnt", "GJ_button_06.png", 16.0f, 0.4f);
            auto delBtn = CCMenuItemExt::createSpriteExtra(delSpr, [this, path](CCObject*) {
                std::error_code ec2;
                std::filesystem::remove(path, ec2);
                this->refreshMacroList();
                Notification::create("Macro deleted", NotificationIcon::Info)->show();
            });
            delBtn->setPosition(145.0f, rowH / 2.0f);
            rowMenu->addChild(delBtn);

            rowNode->addChild(rowMenu);
            scroll->m_contentLayer->addChild(rowNode);
        }

        scroll->scrollToTop();
        m_fields->m_fileListContainer->addChild(scroll);
    }

    void customSetup() {
        PauseLayer::customSetup();
        if (!PlayLayer::get()) return;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        float panelW = 170.0f;
        float panelH = 282.0f;
        float panelX = winSize.width - panelW - 8.0f;
        float panelY = (winSize.height - panelH) / 2.0f;

        auto panelBg = CCLayerColor::create(ccc4(0, 0, 0, 190), panelW, panelH);
        panelBg->setPosition(panelX, panelY);
        this->addChild(panelBg, 100);

        auto titleLbl = CCLabelBMFont::create("CBF TAS BOT", "goldFont.fnt");
        titleLbl->setScale(0.5f);
        titleLbl->setPosition(panelW / 2.0f, panelH - 16.0f);
        panelBg->addChild(titleLbl);

        m_fields->m_statusLabel = CCLabelBMFont::create("OFF", "chatFont.fnt");
        m_fields->m_statusLabel->setScale(0.5f);
        m_fields->m_statusLabel->setPosition(panelW / 2.0f, panelH - 34.0f);
        panelBg->addChild(m_fields->m_statusLabel);

        auto modeMenu = CCMenu::create();
        modeMenu->setPosition(0.0f, 0.0f);

        m_fields->m_offSprite = ButtonSprite::create("OFF", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto offBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_offSprite, [this](CCObject*) {
            bot::setMode(bot::Mode::Off);
            this->updateStatusUI();
        });
        offBtn->setPosition(panelW * 0.2f, panelH - 58.0f);
        modeMenu->addChild(offBtn);

        m_fields->m_recSprite = ButtonSprite::create("REC", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto recBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_recSprite, [this](CCObject*) {
            bot::macro.clear();
            bot::setMode(bot::Mode::Record);
            this->updateStatusUI();
            Notification::create("Recording started", NotificationIcon::Info)->show();
            this->onRestart(nullptr);
        });
        recBtn->setPosition(panelW * 0.5f, panelH - 58.0f);
        modeMenu->addChild(recBtn);

        m_fields->m_playSprite = ButtonSprite::create("PLAY", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto playBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_playSprite, [this](CCObject*) {
            if (bot::macro.empty()) {
                Notification::create("No macro!", NotificationIcon::Warning)->show();
                return;
            }
            bot::setMode(bot::Mode::Play);
            this->updateStatusUI();
            Notification::create("Playing...", NotificationIcon::Success)->show();
            this->onRestart(nullptr);
        });
        playBtn->setPosition(panelW * 0.8f, panelH - 58.0f);
        modeMenu->addChild(playBtn);

        panelBg->addChild(modeMenu);

        auto speedLbl = CCLabelBMFont::create("Speed:", "chatFont.fnt");
        speedLbl->setScale(0.45f);
        speedLbl->setAnchorPoint(CCPoint{ 0.0f, 0.5f });
        speedLbl->setPosition(10.0f, panelH - 84.0f);
        panelBg->addChild(speedLbl);

        auto speedInput = TextInput::create(65.0f, "1.00", "chatFont.fnt");
        speedInput->setFilter("0123456789.,");
        speedInput->setScale(0.7f);
        speedInput->setPosition(100.0f, panelH - 84.0f);
        speedInput->setCallback([](std::string const& str) {
            bot::recSpeed = bot::parseSpeedString(str);
        });
        speedInput->setString(fmt::format("{:.2f}", bot::recSpeed));
        panelBg->addChild(speedInput);

        auto fxMenu = CCMenu::create();
        fxMenu->setPosition(0.0f, 0.0f);
        m_fields->m_fxSprite = ButtonSprite::create("FX", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto fxBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_fxSprite, [this](CCObject*) {
            bot::disableDeathEffects = !bot::disableDeathEffects;
            Mod::get()->setSavedValue("disableDeathEffects", bot::disableDeathEffects);
            this->updateStatusUI();
            Notification::create(
                bot::disableDeathEffects ? "Death, Wave & Respawn FX: OFF" : "Death, Wave & Respawn FX: ON",
                NotificationIcon::Info
            )->show();
        });
        fxBtn->setPosition(145.0f, panelH - 84.0f);
        fxMenu->addChild(fxBtn);
        panelBg->addChild(fxMenu);

        auto nameLbl = CCLabelBMFont::create("Name:", "chatFont.fnt");
        nameLbl->setScale(0.45f);
        nameLbl->setAnchorPoint(CCPoint{ 0.0f, 0.5f });
        nameLbl->setPosition(10.0f, panelH - 108.0f);
        panelBg->addChild(nameLbl);

        m_fields->m_nameInput = TextInput::create(110.0f, bot::generateTimestampName().c_str(), "chatFont.fnt");
        m_fields->m_nameInput->setScale(0.7f);
        m_fields->m_nameInput->setPosition(105.0f, panelH - 108.0f);
        panelBg->addChild(m_fields->m_nameInput);

        auto saveMenu = CCMenu::create();
        saveMenu->setPosition(0.0f, 0.0f);
        auto saveSpr = ButtonSprite::create("SAVE CURRENT", 80, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](CCObject*) {
            if (bot::macro.empty()) {
                Notification::create("Macro is empty", NotificationIcon::Warning)->show();
                return;
            }

            std::string raw = m_fields->m_nameInput
                ? std::string(m_fields->m_nameInput->getString())
                : std::string();
            std::string clean = bot::sanitizeFileName(raw);
            std::string stem = clean.empty() ? bot::generateTimestampName() : clean;
            std::string filename = stem + ".gdbot";

            auto path = bot::getMacroDir() / filename;
            bool existed = std::filesystem::exists(path);

            if (bot::saveMacro(path, bot::macro)) {
                this->refreshMacroList();
                Notification::create(
                    existed ? "Macro overwritten" : "Saved macro",
                    NotificationIcon::Success
                )->show();
            }
            else {
                Notification::create("Save failed", NotificationIcon::Error)->show();
            }
        });
        saveBtn->setPosition(panelW / 2.0f, panelH - 134.0f);
        saveMenu->addChild(saveBtn);
        panelBg->addChild(saveMenu);

        m_fields->m_fileListContainer = CCNode::create();
        m_fields->m_fileListContainer->setPosition(5.0f, 15.0f);
        m_fields->m_fileListContainer->setContentSize(CCSize{ 160.0f, 100.0f });
        panelBg->addChild(m_fields->m_fileListContainer);

        this->updateStatusUI();
        this->refreshMacroList();
    }
};
