// CBF-style TAS bot for Geometry Dash 2.2081 / Geode 5.x (Android & Windows)
// 100% Deterministic Replay + Fixed Fast Spam + Seamless Checkpoint Splicing

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>

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

namespace bot {

    constexpr double SMALLEST = 1e-9;
    constexpr double MIN_PART = 1e-8;
    constexpr uint32_t kMagic = 0x32544F42; // "BOT2"

    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t step = 0;
        double frac = 0.0;
        uint8_t button = 1;
        bool down = false;
        bool p1 = true;
    };

    struct Sub {
        bool has = false;
        int button = 1;
        bool push = false;
        bool p1 = true;
        double deltaFactor = 1.0;
        bool endStep = true;
    };

    struct CheckpointData {
        uint32_t step = 0;
        size_t macroSize = 0;
        bool held[4][2] = {};
    };

    // ---- состояние ----
    inline Mode mode = Mode::Off;
    inline std::vector<Action> macro;
    inline uint32_t step = 0;
    inline size_t playIndex = 0;
    inline int applying = 0;
    inline int silent = 0;
    inline uint32_t applyStep = 0;
    inline double applyFrac = 0.0;
    inline bool cpLoaded = false;
    inline std::unordered_map<CheckpointObject*, CheckpointData> checkpoints;
    inline bool currentHeld[4][2] = {};
    inline double recSpeed = 1.0;
    inline float appliedScale = 1.0f;

    // ---- движок ввода и нарезки ----
    inline std::vector<PlayerButtonCommand> inputVector;
    inline double lastPhysicsStepTime = 0.0;
    inline double currentFrameTime = 0.0;
    inline double lastFrameTime = 0.0;
    inline bool zeroFrame = false;
    inline bool inputThisStep = false;
    inline bool p2HandledThisStep = false;
    inline bool p1Split = false;
    inline bool p2Split = false;
    inline bool midStep = false;
    inline CCPoint p1Pos = { 0.f, 0.f };
    inline CCPoint p2Pos = { 0.f, 0.f };
    inline float rotationDelta = 0.f;

    inline double getCurrentTimestamp() {
#ifdef GEODE_IS_WINDOWS
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
#else
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1'000'000'000.0;
#endif
    }

    inline bool isPlayLayer(GJBaseGameLayer* l) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == l;
    }

    inline void resetEngine() {
        inputVector.clear();
        lastPhysicsStepTime = 0.0;
        lastFrameTime = currentFrameTime;
        inputThisStep = false;
        p2HandledThisStep = false;
        p1Split = false;
        p2Split = false;
        midStep = false;
    }

    inline void applyClickFlags() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (mode == Mode::Record) {
            pl->m_clickBetweenSteps = true;
            pl->m_clickOnSteps = true;
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

    inline void setMode(Mode m) {
        mode = m;
        applyClickFlags();
        resetEngine();
    }

    inline void setTimeScale(float s) {
        if (std::fabs(s - appliedScale) < 1e-6f) return;
        appliedScale = s;
        CCDirector::sharedDirector()->getScheduler()->setTimeScale(s);
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
        return std::clamp(whole + frac, 0.0001, 2.0);
    }

    inline std::filesystem::path getMacroDir() {
        auto dir = Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
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

    struct Stats {
        size_t pushes = 0;
        size_t releases = 0;
        double minHold = -1.0;
    };

    inline Stats macroStats() {
        Stats st;
        double pushAt[4][2] = {};
        bool held[4][2] = {};
        for (auto const& a : macro) {
            if (a.button > 3) continue;
            int p = a.p1 ? 0 : 1;
            double t = static_cast<double>(a.step) + a.frac;
            if (a.down) {
                ++st.pushes;
                held[a.button][p] = true;
                pushAt[a.button][p] = t;
            }
            else {
                ++st.releases;
                if (held[a.button][p]) {
                    double d = t - pushAt[a.button][p];
                    if (st.minHold < 0.0 || d < st.minHold) st.minHold = d;
                    held[a.button][p] = false;
                }
            }
        }
        return st;
    }

    inline void applyInput(PlayLayer* pl, Sub const& s, uint32_t st, double frac) {
        ++applying;
        applyStep = st;
        applyFrac = std::clamp(frac, 0.0, 1.0);
        pl->handleButton(s.push, s.button, s.p1);
        --applying;
    }

    inline void silentReleaseAll(PlayLayer* pl) {
        ++silent;
        for (int b = 1; b <= 3; ++b) {
            pl->handleButton(false, b, true);
            pl->handleButton(false, b, false);
            currentHeld[b][0] = false;
            currentHeld[b][1] = false;
        }
        --silent;
    }

    inline void syncPlayback(PlayLayer* pl) {
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
        ++silent;
        for (int b = 1; b <= 3; ++b) {
            currentHeld[b][0] = held[b][0];
            currentHeld[b][1] = held[b][1];
            pl->handleButton(held[b][0], b, true);
            pl->handleButton(held[b][1], b, false);
        }
        --silent;
    }

    inline void onFrame(PlayLayer* pl, double modifiedDelta) {
        if (mode == Mode::Play) {
            pl->m_queuedButtons.clear();
            return;
        }

        if (pl->m_playerDied) {
            inputVector.clear();
            pl->m_queuedButtons.clear();
            lastPhysicsStepTime = 0.0;
            return;
        }

        for (auto cmd : pl->m_queuedButtons) {
            double ts = cmd.m_timestamp;
            if (ts > currentFrameTime + 5.0) ts /= 1000.0;
            if (!(ts > lastFrameTime - 10.0) || ts > currentFrameTime + 1.0) ts = currentFrameTime;
            if (!inputVector.empty() && ts < inputVector.back().m_timestamp) ts = inputVector.back().m_timestamp;
            cmd.m_timestamp = ts;
            inputVector.push_back(cmd);
        }
        pl->m_queuedButtons.clear();
        lastFrameTime = currentFrameTime;
    }

    inline void onFrameStart() {
        currentFrameTime = getCurrentTimestamp();

        PlayLayer* pl = PlayLayer::get();
        CCNode* par = nullptr;
        bool idle = !pl
            || !(par = pl->getParent())
            || par->getChildByType<PauseLayer>(0)
            || pl->getChildByType<EndLevelLayer>(0);

        setTimeScale((mode == Mode::Record && !idle) ? static_cast<float>(recSpeed) : 1.0f);

        if (mode == Mode::Off) return;

        if (idle) {
            lastFrameTime = currentFrameTime;
            lastPhysicsStepTime = 0.0;
            inputVector.clear();
        }
    }

    inline void resetCollisionLog(PlayerObject* p) {
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
// Scheduler
// ---------------------------------------------------------------------------
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        bot::onFrameStart();
        CCScheduler::update(dt);
    }
};

// ---------------------------------------------------------------------------
// Ввод
// ---------------------------------------------------------------------------
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        using namespace bot;

        if (button <= 3) {
            currentHeld[button][isPlayer1 ? 0 : 1] = down;
        }

        if (mode == Mode::Off || silent > 0 || !isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (mode == Mode::Play) {
            if (applying > 0) GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Запись только в момент выполнения CBF-подшага
        if (applying > 0) {
            Action a;
            a.step = applyStep;
            a.frac = applyFrac;
            a.button = static_cast<uint8_t>(button);
            a.down = down;
            a.p1 = isPlayer1;
            macro.push_back(a);
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    double getModifiedDelta(float dt) {
        double md = GJBaseGameLayer::getModifiedDelta(dt);
        if (bot::isPlayLayer(this)) {
            bot::zeroFrame = !(md > 0.0);
            if (bot::mode != bot::Mode::Off) bot::onFrame(PlayLayer::get(), md);
        }
        return md;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (bot::mode != bot::Mode::Off && bot::zeroFrame && bot::isPlayLayer(this)) {
            std::vector<PlayerButtonCommand> saved(this->m_queuedButtons.begin(), this->m_queuedButtons.end());
            this->m_queuedButtons.clear();
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            for (auto const& c : saved) this->m_queuedButtons.push_back(c);
            return;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------------------
// Нарезка подшагов строго во время физического шага (100% точность)
// ---------------------------------------------------------------------------
class $modify(BotPlayerObject, PlayerObject) {
    void update(float stepDelta) {
        using namespace bot;
        auto pl = PlayLayer::get();
        if (!pl || midStep) {
            PlayerObject::update(stepDelta);
            return;
        }

        if (zeroFrame || stepDelta <= 1e-9f) {
            PlayerObject::update(stepDelta);
            return;
        }

        if (mode == Mode::Off) {
            PlayerObject::update(stepDelta);
            if (this == pl->m_player1) ++step;
            return;
        }

        if (this != pl->m_player1) {
            if (this == pl->m_player2 && p2HandledThisStep) {
                p2HandledThisStep = false;
                return;
            }
            PlayerObject::update(stepDelta);
            return;
        }

        uint32_t const cur = step;
        std::vector<Sub> subs;

        if (mode == Mode::Play) {
            double elapsed = 0.0;
            while (playIndex < macro.size() && macro[playIndex].step <= cur) {
                auto const& a = macro[playIndex++];
                double f = (a.step < cur) ? 0.0 : std::clamp(a.frac, 0.0, 1.0);
                Sub s;
                s.has = true;
                s.button = a.button;
                s.push = a.down;
                s.p1 = a.p1;
                s.deltaFactor = std::clamp(f - elapsed, SMALLEST, 1.0);
                s.endStep = false;
                subs.push_back(s);
                elapsed = std::max(elapsed, f);
            }
            if (!subs.empty()) {
                Sub e;
                e.deltaFactor = std::max(SMALLEST, 1.0 - elapsed);
                e.endStep = true;
                subs.push_back(e);
            }
        }
        else { // Mode::Record
            double now = getCurrentTimestamp();
            double expectedStepReal = static_cast<double>(stepDelta) / std::max(0.0001, (double)appliedScale);

            if (lastPhysicsStepTime <= 0.0 || (now - lastPhysicsStepTime) > expectedStepReal * 3.5) {
                lastPhysicsStepTime = now - expectedStepReal;
            }

            double stepSpan = now - lastPhysicsStepTime;
            if (stepSpan <= 0.0) stepSpan = expectedStepReal;

            size_t consumed = 0;
            double elapsed = 0.0;

            while (consumed < inputVector.size()) {
                auto const& in = inputVector[consumed];
                double rel = in.m_timestamp - lastPhysicsStepTime;

                if (rel <= stepSpan || in.m_timestamp <= now || consumed == 0) {
                    double t = std::clamp(rel / stepSpan, 0.0, 1.0);
                    Sub s;
                    s.has = true;
                    s.button = static_cast<int>(in.m_button);
                    s.push = in.m_isPush;
                    s.p1 = !in.m_isPlayer2;
                    s.deltaFactor = std::clamp(t - elapsed, SMALLEST, 1.0);
                    s.endStep = false;
                    subs.push_back(s);
                    elapsed = std::max(elapsed, t);
                    ++consumed;
                }
                else break;
            }

            if (!subs.empty()) {
                Sub e;
                e.deltaFactor = std::max(SMALLEST, 1.0 - elapsed);
                e.endStep = true;
                subs.push_back(e);
            }

            inputVector.erase(inputVector.begin(), inputVector.begin() + consumed);
            lastPhysicsStepTime = now;
        }

        inputThisStep = !subs.empty();

        if (subs.empty()) {
            p1Split = false;
            p2Split = false;
            p2HandledThisStep = false;
            PlayerObject::update(stepDelta);
            ++step;
            return;
        }

        PlayerObject* p2 = pl->m_player2;
        bool isDual = pl->m_gameState.m_isDualMode;
        bool p1StartedOnGround = this->m_isOnGround;
        bool p2StartedOnGround = p2 && p2->m_isOnGround;

        // В режиме PLAY гарантируем выполнение сплита
        if (mode == Mode::Play) {
            p1Split = true;
            p2Split = isDual;
        } else {
            bool p1NotBuffering = p1StartedOnGround
                || this->m_touchingRings->count()
                || this->m_isDashing
                || (this->m_isDart || this->m_isBird || this->m_isShip || this->m_isSwing);

            bool p2NotBuffering = p2 && (p2StartedOnGround
                || p2->m_touchingRings->count()
                || p2->m_isDashing
                || (p2->m_isDart || p2->m_isBird || p2->m_isShip || p2->m_isSwing));

            p1Split = p1NotBuffering;
            p2Split = p2NotBuffering && isDual;
        }

        p1Pos = PlayerObject::getPosition();
        if (p2) p2Pos = p2->getPosition();

        midStep = true;
        bool firstLoop = true;
        double acc = 0.0;
        double carry = 0.0;
        float spentStepDelta = 0.0f;
        Sub pending;

        for (auto const& s : subs) {
            if (pending.has) {
                double at = (p1Split || p2Split) ? acc : 0.0;
                applyInput(pl, pending, cur, at);
            }
            pending = s;

            double const part = s.deltaFactor + carry;
            if (!s.endStep && part < MIN_PART && !pending.has) {
                carry = part;
                acc += s.deltaFactor;
                continue;
            }
            carry = 0.0;

            float substepDelta;
            if (s.endStep) {
                substepDelta = std::max(0.0f, stepDelta - spentStepDelta);
            } else {
                substepDelta = static_cast<float>(stepDelta * part);
                spentStepDelta += substepDelta;
            }
            rotationDelta = substepDelta;

            if (p1Split) {
                PlayerObject::update(substepDelta);
                if (!s.endStep) {
                    if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) this->m_isOnGround = p1StartedOnGround;
                    if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true);
                    else pl->checkCollisions(this, stepDelta, true);
                    PlayerObject::updateRotation(substepDelta);
                    bot::resetCollisionLog(this);
                }
            }
            else if (s.endStep) PlayerObject::update(stepDelta);

            if (p2) {
                if (p2Split) {
                    p2->update(substepDelta);
                    if (!s.endStep) {
                        if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) p2->m_isOnGround = p2StartedOnGround;
                        if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
                        else pl->checkCollisions(p2, stepDelta, true);
                        p2->updateRotation(substepDelta);
                        bot::resetCollisionLog(p2);
                    }
                }
                else if (s.endStep) p2->update(stepDelta);
            }

            acc += s.deltaFactor;
            firstLoop = false;
        }

        p2HandledThisStep = true;
        midStep = false;
        ++step;
    }

    void updateRotation(float t) {
        auto pl = PlayLayer::get();
        if (bot::zeroFrame) {
            PlayerObject::updateRotation(t);
            return;
        }
        if (pl && bot::mode != bot::Mode::Off && !bot::midStep) {
            if (this == pl->m_player1 && bot::p1Split) {
                PlayerObject::updateRotation(bot::rotationDelta);
                this->m_lastPosition = bot::p1Pos;
                return;
            }
            if (this == pl->m_player2 && bot::p2Split) {
                PlayerObject::updateRotation(bot::rotationDelta);
                this->m_lastPosition = bot::p2Pos;
                return;
            }
        }
        PlayerObject::updateRotation(t);
    }
};

// ---------------------------------------------------------------------------
// Уровень, чекпоинты (Бесшовное склеивание макроса)
// ---------------------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bot::mode = bot::Mode::Off;
        bot::step = 0;
        bot::playIndex = 0;
        bot::checkpoints.clear();
        bot::resetEngine();
        srand(1337);

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            Notification::create("Disable Click Between Frames mod: it conflicts with the bot", NotificationIcon::Warning, 4.f)->show();
        }
        return true;
    }

    void resetLevel() {
        bot::cpLoaded = false;
        PlayLayer::resetLevel();
        bot::resetEngine();
        srand(1337);
        if (bot::cpLoaded) return;

        bot::step = 0;
        bot::playIndex = 0;
        bot::checkpoints.clear();
        if (bot::mode == bot::Mode::Record) bot::macro.clear();
        if (bot::mode != bot::Mode::Off) bot::silentReleaseAll(this);
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (cp) {
            bot::CheckpointData data;
            data.step = bot::step;
            data.macroSize = bot::macro.size(); // Сохраняем точный размер макроса для бесшовного среза
            for (int b = 1; b <= 3; ++b) {
                data.held[b][0] = bot::currentHeld[b][0];
                data.held[b][1] = bot::currentHeld[b][1];
            }
            bot::checkpoints[cp] = data;
        }
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        bot::cpLoaded = true;
        bot::resetEngine();

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

        auto const& data = it->second;
        bot::step = data.step;

        // 1. ИДЕАЛЬНОЕ СКЛЕИВАНИЕ МАКРОСА: срез строго до сохранённого момента чекпоинта
        if (bot::mode == bot::Mode::Record) {
            if (data.macroSize <= bot::macro.size()) {
                bot::macro.erase(bot::macro.begin() + data.macroSize, bot::macro.end());
            } else {
                uint32_t s = bot::step;
                bot::macro.erase(
                    std::remove_if(bot::macro.begin(), bot::macro.end(), [s](bot::Action const& a) { return a.step >= s; }),
                    bot::macro.end()
                );
            }
        }
        else if (bot::mode == bot::Mode::Play) {
            bot::syncPlayback(this);
        }

        // 2. ВОССТАНОВЛЕНИЕ ЗАЖАТИЙ КНОПОК: волна/корабль не падают вниз при возрождении
        ++bot::silent;
        for (int b = 1; b <= 3; ++b) {
            bot::currentHeld[b][0] = data.held[b][0];
            bot::currentHeld[b][1] = data.held[b][1];
            this->handleButton(data.held[b][0], b, true);
            this->handleButton(data.held[b][1], b, false);
        }
        --bot::silent;

        // 3. УСТРАНЕНИЕ ЛАГА ВОЗРОЖДЕНИЯ: чистый якорь времени для первого шага
        bot::lastPhysicsStepTime = bot::getCurrentTimestamp();
        bot::lastFrameTime = bot::lastPhysicsStepTime;
        bot::inputVector.clear();
        this->m_queuedButtons.clear();

        // 4. ДЕТЕРМИНИЗМ РАНДОМА: одинаковое поведение триггеров
        srand(1337 + data.step);
    }
};

// ---------------------------------------------------------------------------
// Меню паузы
// ---------------------------------------------------------------------------
class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCNode* m_fileListContainer = nullptr;
        CCLabelBMFont* m_statusLabel = nullptr;
        ButtonSprite* m_offSprite = nullptr;
        ButtonSprite* m_recSprite = nullptr;
        ButtonSprite* m_playSprite = nullptr;
    };

    void updateStatusUI() {
        if (m_fields->m_statusLabel) {
            auto stats = bot::macroStats();
            std::string minStr = stats.minHold >= 0.0 ? fmt::format("{:.4f}", stats.minHold) : std::string("-");
            std::string info = fmt::format("{} acts P{}/R{} min {}", bot::macro.size(), stats.pushes, stats.releases, minStr);
            std::string st = "OFF";
            if (bot::mode == bot::Mode::Record) st = "REC " + info;
            else if (bot::mode == bot::Mode::Play) st = "PLAY " + info;
            else if (!bot::macro.empty()) st = "OFF " + info;
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
        float panelH = 260.0f;
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
        speedInput->setString(fmt::format("{}", bot::recSpeed));
        panelBg->addChild(speedInput);

        auto saveMenu = CCMenu::create();
        saveMenu->setPosition(0.0f, 0.0f);
        auto saveSpr = ButtonSprite::create("SAVE CURRENT", 80, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](CCObject*) {
            if (bot::macro.empty()) {
                Notification::create("Macro is empty", NotificationIcon::Warning)->show();
                return;
            }
            auto now = std::chrono::system_clock::now();
            auto timeT = std::chrono::system_clock::to_time_t(now);
            std::tm tmStruct{};
#ifdef GEODE_IS_WINDOWS
            localtime_s(&tmStruct, &timeT);
#else
            localtime_r(&timeT, &tmStruct);
#endif
            std::string filename = fmt::format(
                "macro_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}.gdbot",
                tmStruct.tm_year + 1900, tmStruct.tm_mon + 1, tmStruct.tm_mday,
                tmStruct.tm_hour, tmStruct.tm_min, tmStruct.tm_sec
            );
            if (bot::saveMacro(bot::getMacroDir() / filename, bot::macro)) {
                this->refreshMacroList();
                Notification::create("Saved macro", NotificationIcon::Success)->show();
            }
            else {
                Notification::create("Save failed", NotificationIcon::Error)->show();
            }
        });
        saveBtn->setPosition(panelW / 2.0f, panelH - 110.0f);
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
