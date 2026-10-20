// CBF-style TAS bot for Geometry Dash 2.2081 / Geode 5.x (Android & PC)
//
// Ключевая идея CBF: реальный ввод перехватывается ДО того, как игра
// положит его в очередь тика (m_queuedButtons), а применяется внутри шага
// через субтиковую симуляцию в PlayerObject::update. Это даёт точность
// меньше одного тика и в записи, и в воспроизведении.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <fmod.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Разблокировка предметов (сохранение игры не модифицируется)
// ---------------------------------------------------------------------------
class $modify(BotGameManager, GameManager) {
    bool isIconUnlocked(int id, IconType type) { return true; }
    bool isColorUnlocked(int id, UnlockType type) { return true; }
};

class $modify(BotGameStatsManager, GameStatsManager) {
    bool isItemUnlocked(UnlockType type, int id) { return true; }
};

// ---------------------------------------------------------------------------
// Общее состояние настроек (объявлено до хуков)
// ---------------------------------------------------------------------------
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

namespace bot {

    constexpr uint32_t kMagic = 0x32544F42; // "BOT2"
    constexpr uint32_t kMaxActions = 2'000'000;
    constexpr std::uintmax_t kMaxFileSize = 64ull * 1024ull * 1024ull;

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
        double timestamp = 0.0;
    };

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

    inline std::vector<RawInput> pendingRecordInputs;
    inline double g_stepStartTime = 0.0;

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
        g_stepStartTime = 0.0;
        inputThisStep = false;
        p1Split = false;
        p2Split = false;
        midStep = false;
        rotationDelta = 0.f;
    }

    // ВАЖНО: в Record выключаем clickBetweenSteps. Очередь тика нам не нужна —
    // клики приходят из диспетчера ввода и применяются субтиково вручную.
    // Если оставить true, игра сама дренирует очередь на границах шага,
    // и время клика квантуется до TPS.
    inline void applyClickFlags() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (mode == Mode::Record || mode == Mode::Play) {
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
        if (ec) return Mod::get()->getSaveDir();
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
        static const char* reserved[] = {
            "CON","PRN","AUX","NUL",
            "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
            "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"
        };
        for (auto const* r : reserved) {
            if (upper == r) { out = "_" + out; break; }
        }
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
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        if (ec || size > kMaxFileSize) return false;

        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        uint32_t magic = 0, count = 0;
        in.read(reinterpret_cast<char*>(&magic), 4);
        in.read(reinterpret_cast<char*>(&count), 4);
        if (!in || magic != kMagic || count > kMaxActions) return false;
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
            g_stepStartTime = 0.0;
        }
    }

    // -----------------------------------------------------------------------
    // Маппинг клавиш / тача в кнопки игрока.
    // По умолчанию в GD: Space и стрелка вверх — прыжок P1, стрелка вниз —
    // P2 (в двойном режиме). Игроки переназначают клавиши в настройках,
    // поэтому при необходимости добавьте свои комбинации.
    // -----------------------------------------------------------------------
    inline int keyToButtonPC(enumKeyCodes key, bool& outP1) {
        switch (key) {
            case KEY_Space: outP1 = true;  return 1;
            case KEY_Up:    outP1 = true;  return 1;
            case KEY_Down:  outP1 = false; return 2;
            default: break;
        }
        return 0;
    }

    inline int touchToButton(float x, float width, bool& outP1) {
        bool leftHalf = x < width * 0.5f;
        outP1 = leftHalf;
        return leftHalf ? 1 : 2;
    }
}

// ---------------------------------------------------------------------------
// Эффект волны смерти
// ---------------------------------------------------------------------------
class $modify(BotCircleWave, CCCircleWave) {
    void update(float dt) {
        if (bot::disableDeathEffects && bot::mode != bot::Mode::Off) {
            this->setVisible(false);
            return;
        }
        CCCircleWave::update(dt);
    }
};

// ---------------------------------------------------------------------------
// Контроллер планировщика
// ---------------------------------------------------------------------------
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        bot::onFrameStart();
        CCScheduler::update(dt);
        bot::updateAudioPitch();
    }
};

// ---------------------------------------------------------------------------
// ПЕРЕХВАТ ВВОДА (PC/Android). Стоит ВЫШЕ handleButton, поэтому время
// нажатия ещё не квантовано тиком. Именно этого не хватало для CBF.
// ---------------------------------------------------------------------------
class $modify(BotKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool isKeyRepeat) {
        using namespace bot;

        if (mode == Mode::Record && !isKeyRepeat && bot::recordingActive()) {
            bool p1 = true;
            int btn = keyToButtonPC(key, p1);
            if (btn > 0) {
                recordRawInput(btn, down, p1, getCurrentTimestamp());
                return true; // поглотить: в игру не пускаем
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, isKeyRepeat);
    }
};

// Тач-ввод-фокусированная часть: работает и на Android, и на PC с тач-экраном.
// Хук CCTouchDispatcher, а не PlayLayer::ccTouchBegan: там уже могут быть
// игровые задержки. CCTouchDispatcher — самая ранняя из доступных точек.
class $modify(BotTouchDispatcher, CCTouchDispatcher) {
    void touchesBegan(CCSet* touches, CCEvent* event) {
        using namespace bot;
        if (mode == Mode::Record && bot::recordingActive()) {
            auto pl = PlayLayer::get();
            auto win = CCDirector::sharedDirector()->getWinSize();
            for (auto* obj : *touches) {
                auto touch = static_cast<CCTouch*>(obj);
                auto loc = touch->getLocation();
                if (loc.y < win.height * 0.15f) continue; // панель UI снизу — пропускаем
                bool p1 = true;
                int btn = touchToButton(loc.x, win.width, p1);
                recordRawInput(btn, true, p1, getCurrentTimestamp());
            }
            return; // в игру тач не пускаем
        }
        CCTouchDispatcher::touchesBegan(touches, event);
    }

    void touchesEnded(CCSet* touches, CCEvent* event) {
        using namespace bot;
        if (mode == Mode::Record && bot::recordingActive()) {
            auto win = CCDirector::sharedDirector()->getWinSize();
            for (auto* obj : *touches) {
                auto touch = static_cast<CCTouch*>(obj);
                auto loc = touch->getLocation();
                if (loc.y < win.height * 0.15f) continue;
                bool p1 = true;
                int btn = touchToButton(loc.x, win.width, p1);
                recordRawInput(btn, false, p1, getCurrentTimestamp());
            }
            return;
        }
        CCTouchDispatcher::touchesEnded(touches, event);
    }
};

// ---------------------------------------------------------------------------
// Применение ввода в субтиковый момент. Реальные нажатия сюда уже не
// доходят — они поглощены диспетчерами выше. handleButton остаётся
// для silentReleaseAll и applyInput (флаг applying).
// ---------------------------------------------------------------------------
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        using namespace bot;

        if (mode == Mode::Off || silent > 0 || !isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (applying > 0) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Mode::Record / Mode::Play: реальные нажатия перехвачены выше,
        // сюда попадают только внутренние вызовы — не пропускаем.
    }
};

// ---------------------------------------------------------------------------
// Нарезка подтиков игрока (движок CBF)
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

        if (g_stepStartTime <= 0.0 || (now - g_stepStartTime) > (stepDuration * 4.0) || g_stepStartTime > now) {
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
            for (auto const& in : pendingRecordInputs) {
                if (in.timestamp <= stepEndTime) {
                    StepSub s;
                    s.button = in.button;
                    s.push = in.push;
                    s.p1 = in.p1;

                    double rel = in.timestamp - stepStartTime;
                    s.frac = std::clamp(rel / stepDuration, 0.0, 0.9999);
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
            g_stepStartTime = stepEndTime;
            ++step;
            return;
        }

        std::stable_sort(subs.begin(), subs.end(), [](auto const& a, auto const& b) {
            return a.frac < b.frac;
        });

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
                pl->checkCollisions(this, substepDelta, true);
                PlayerObject::updateRotation(substepDelta);

                if (p2Split && p2) {
                    p2->update(substepDelta);
                    if (i == 0 && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) p2->m_isOnGround = p2StartedOnGround;
                    pl->checkCollisions(p2, substepDelta, true);
                    p2->updateRotation(substepDelta);
                }
            }

            applyInput(pl, s, cur, s.frac);
            lastFrac = s.frac;
        }

        double remainingFactor = std::max(0.0, 1.0 - lastFrac);
        float remainingDelta = static_cast<float>(stepDelta * remainingFactor);
        rotationDelta = remainingDelta;

        if (remainingDelta > 1e-9f) {
            PlayerObject::update(remainingDelta);
            pl->checkCollisions(this, remainingDelta, true);
            PlayerObject::updateRotation(remainingDelta);

            if (p2Split && p2) {
                p2->update(remainingDelta);
                pl->checkCollisions(p2, remainingDelta, true);
                p2->updateRotation(remainingDelta);
            }
        }

        midStep = false;
        g_stepStartTime = stepEndTime;
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
// Контроллер уровня и чекпоинтов
// ---------------------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bot::mode = bot::Mode::Off;
        bot::step = 0;
        bot::playIndex = 0;
        bot::checkpoints.clear();
        bot::resetEngine();
        bot::restoreAudioPitch();

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        if (auto mod = Mod::get()) {
            bot::disableDeathEffects = mod->getSavedValue<bool>("disableDeathEffects", true);
            bot::recSpeed = mod->getSavedValue<double>("recSpeed", 1.0);
        }
        bot::applyClickFlags();

        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            Notification::create("Disable Click Between Frames mod: it conflicts with the bot", NotificationIcon::Warning, 4.f)->show();
        }
        return true;
    }

    void onQuit() {
        bot::setTimeScale(1.0f);
        bot::restoreAudioPitch();
        bot::resetEngine();
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
        bot::g_stepStartTime = 0.0;
        bot::pendingRecordInputs.clear();

        if (bot::mode == bot::Mode::Record) {
            uint32_t s = bot::step;
            auto& m = bot::macro;
            m.erase(std::remove_if(m.begin(), m.end(), [s](bot::Action const& a) { return a.step >= s; }), m.end());
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
        PlayLayer::destroyPlayer(player, object);
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
        TextInput* m_speedInput = nullptr;
        bool m_refreshScheduled = false;
    };

    void updateStatusUI() {
        if (m_fields->m_statusLabel) {
            std::string st = "OFF";
            if (bot::mode == bot::Mode::Record) st = fmt::format("REC ({} acts)", bot::macro.size());
            else if (bot::mode == bot::Mode::Play) st = fmt::format("PLAY ({} acts)", bot::macro.size());
            else if (!bot::macro.empty()) st = fmt::format("OFF ({} acts loaded)", bot::macro.size());
            m_fields->m_statusLabel->setString(st.c_str());
        }
        if (m_fields->m_offSprite) m_fields->m_offSprite->setColor(bot::mode == bot::Mode::Off ? ccColor3B{80,255,80} : ccColor3B{255,255,255});
        if (m_fields->m_recSprite) m_fields->m_recSprite->setColor(bot::mode == bot::Mode::Record ? ccColor3B{255,80,80} : ccColor3B{255,255,255});
        if (m_fields->m_playSprite) m_fields->m_playSprite->setColor(bot::mode == bot::Mode::Play ? ccColor3B{80,200,255} : ccColor3B{255,255,255});
        if (m_fields->m_fxSprite) m_fields->m_fxSprite->setColor(bot::disableDeathEffects ? ccColor3B{255,180,80} : ccColor3B{255,255,255});
    }

    void refreshMacroListDeferred() {
        m_fields->m_refreshScheduled = false;
        this->refreshMacroList();
    }

        void scheduleRefreshMacroList() {
        if (m_fields->m_refreshScheduled) return;
        m_fields->m_refreshScheduled = true;
        this->runAction(CCSequence::createWithTwoActions(
            CCDelayTime::create(0.0f),
            CCCallFunc::create(this, callfunc_selector(BotPauseLayer::refreshMacroListDeferred))
        ));
    }

    void refreshMacroList() {
        if (!m_fields->m_fileListContainer) return;
        m_fields->m_fileListContainer->removeAllChildren();

        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (auto const& entry : std::filesystem::directory_iterator(bot::getMacroDir(), ec)) {
            if (ec) break;
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

        auto rowMenu = CCMenu::create();
        rowMenu->setPosition(0.0f, 0.0f);
        rowMenu->setContentSize(CCSize{ viewW, totalH });
        scroll->m_contentLayer->addChild(rowMenu);

        for (size_t i = 0; i < files.size(); ++i) {
            auto path = files[i];
            float y = totalH - static_cast<float>(i + 1) * rowH;

            std::string name = path.stem().string();
            if (name.length() > 14) name = name.substr(0, 12) + "..";
            auto lbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
            lbl->setScale(0.4f);
            lbl->setAnchorPoint(CCPoint{ 0.0f, 0.5f });
            lbl->setPosition(4.0f, y + rowH / 2.0f);
            scroll->m_contentLayer->addChild(lbl);

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
            loadBtn->setPosition(120.0f, y + rowH / 2.0f);
            rowMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("X", 12, true, "goldFont.fnt", "GJ_button_06.png", 16.0f, 0.4f);
            auto delBtn = CCMenuItemExt::createSpriteExtra(delSpr, [this, path](CCObject*) {
                std::error_code ec2;
                std::filesystem::remove(path, ec2);
                this->scheduleRefreshMacroList();
                Notification::create("Macro deleted", NotificationIcon::Info)->show();
            });
            delBtn->setPosition(145.0f, y + rowH / 2.0f);
            rowMenu->addChild(delBtn);
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
            if (auto pl = PlayLayer::get()) pl->resetLevel();
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
            if (auto pl = PlayLayer::get()) pl->resetLevel();
        });
        playBtn->setPosition(panelW * 0.8f, panelH - 58.0f);
        modeMenu->addChild(playBtn);

        panelBg->addChild(modeMenu);

        auto speedLbl = CCLabelBMFont::create("Speed:", "chatFont.fnt");
        speedLbl->setScale(0.45f);
        speedLbl->setAnchorPoint(CCPoint{ 0.0f, 0.5f });
        speedLbl->setPosition(10.0f, panelH - 84.0f);
        panelBg->addChild(speedLbl);

        m_fields->m_speedInput = TextInput::create(65.0f, "1.00", "chatFont.fnt");
        m_fields->m_speedInput->setFilter("0123456789.,");
        m_fields->m_speedInput->setScale(0.7f);
        m_fields->m_speedInput->setPosition(100.0f, panelH - 84.0f);
        m_fields->m_speedInput->setCallback([](std::string const& str) {
            bot::recSpeed = bot::parseSpeedString(str);
            if (auto mod = Mod::get()) mod->setSavedValue("recSpeed", bot::recSpeed);
        });
        m_fields->m_speedInput->setString(fmt::format("{:.2f}", bot::recSpeed));
        panelBg->addChild(m_fields->m_speedInput);

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
                this->scheduleRefreshMacroList();
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
