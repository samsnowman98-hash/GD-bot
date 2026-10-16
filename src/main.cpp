// CBF-style TAS bot for Geometry Dash 2.2081 / Geode 5.x (Android)
//
// Макрос хранится как {step, frac}: номер физического шага + доля шага (double).
// Запись берёт реальное время нажатия (m_queuedButtons[i].m_timestamp), раскладывает его по шагам кадра
// и режет шаг на части. Воспроизведение режет шаги точно так же.
// Замедление учитывается автоматически: шаги не зависят от FPS и скорости игры.
//
// ВАЖНО: мод Click Between Frames должен быть ВЫКЛЮЧЕН (оба мода делают одно и то же и мешают друг другу).

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
#include <time.h>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace bot {

    constexpr double SMALLEST = std::numeric_limits<float>::min();
    constexpr double MIN_PART = 5e-4; // минимальная часть шага (доля), короче неразличима в float-координатах
    constexpr uint32_t kMagic = 0x32544F42; // "BOT2"

    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t step = 0;   // номер физического шага от начала попытки
        double frac = 0.0;   // доля шага 0..1
        uint8_t button = 1;
        bool down = false;
        bool p1 = true;
    };

    // Одна часть разрезанного шага: сначала идёт часть длиной deltaFactor, потом применяется клик (если has)
    struct Sub {
        bool has = false;
        int button = 1;
        bool push = false;
        bool p1 = true;
        double deltaFactor = 1.0;
        bool endStep = true;
    };

    // ---- состояние ----
    inline Mode mode = Mode::Off;
    inline std::vector<Action> macro;
    inline uint32_t step = 0;         // сколько физических шагов P1 уже выполнено в попытке
    inline size_t playIndex = 0;      // следующее действие для воспроизведения
    inline int applying = 0;          // >0 пока мы сами применяем клик (записывается / проходит в PLAY)
    inline int silent = 0;            // >0 для служебных нажатий (не записываются)
    inline uint32_t applyStep = 0;
    inline double applyFrac = 0.0;
    inline bool cpLoaded = false;
    inline std::unordered_map<CheckpointObject*, uint32_t> checkpoints;
    inline double recSpeed = 1.0;
    inline float appliedScale = 1.0f;

    // ---- движок нарезки ----
    inline std::deque<Sub> stepQueue;
    inline std::vector<PlayerButtonCommand> inputVector;
    inline double lastFrameTime = 0.0;
    inline double currentFrameTime = 0.0;
    inline bool skipUpdate = true;
    inline bool zeroFrame = false;   // кадр без физических шагов (замедление / очень высокий FPS)
    inline bool inputThisStep = false;
    inline bool p1Split = false;
    inline bool p2Split = false;
    inline bool midStep = false;
    inline CCPoint p1Pos = { 0.f, 0.f };
    inline CCPoint p2Pos = { 0.f, 0.f };
    inline float rotationDelta = 0.f;

    inline double getCurrentTimestamp() {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1'000'000'000.0;
    }

    inline bool isPlayLayer(GJBaseGameLayer* l) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == l;
    }

    inline void resetEngine() {
        stepQueue.clear();
        inputVector.clear();
        // Якорь времени: следующий кадр сразу режется по времени кликов (без потерянного кадра после респавна)
        lastFrameTime = currentFrameTime;
        skipUpdate = true;
        inputThisStep = false;
        p1Split = false;
        p2Split = false;
        midStep = false;
    }

    // REC/PLAY: наши клики вместо родных Click Between/On Steps; OFF: возвращаем настройки игрока
    inline void applyClickFlags() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (mode != Mode::Off) {
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
        return std::clamp(whole + frac, 0.01, 2.0);
    }

    // ---- файлы ----
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

    // ---- кнопки ----
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
        }
        --silent;
    }

    // Перемотка воспроизведения на текущий шаг + восстановление зажатых кнопок
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
            pl->handleButton(held[b][0], b, true);
            pl->handleButton(held[b][1], b, false);
        }
        --silent;
    }

    /*
    Раскладывает реальные клики кадра по физическим шагам кадра.
    Для шага: [часть до клика][клик][часть до след. клика]...[остаток шага]
    Время шага = реальное время кадра / число шагов, поэтому замедление учитывается само.
    */
    inline void buildStepQueue(PlayLayer* pl, int stepCount) {
        stepQueue.clear();
        skipUpdate = false;

        double deltaTime = currentFrameTime - lastFrameTime;
        if (!(deltaTime > 0.0) || stepCount < 1) {
            skipUpdate = true;
            lastFrameTime = currentFrameTime;
            return;
        }

        for (auto cmd : pl->m_queuedButtons) {
            // защита: если время пришло в миллисекундах, переводим в секунды
            if (cmd.m_timestamp > currentFrameTime + 5.0) cmd.m_timestamp /= 1000.0;
            inputVector.push_back(cmd);
        }
        pl->m_queuedButtons.clear();

        double stepDelta = deltaTime / stepCount;
        size_t inputIdx = 0;

        for (int i = 0; i < stepCount; ++i) {
            double elapsed = 0.0;
            while (inputIdx < inputVector.size()) {
                auto const& in = inputVector[inputIdx];
                double rel = in.m_timestamp - lastFrameTime;
                if (rel < stepDelta * (i + 1)) {
                    double t = std::clamp(std::fmod(rel, stepDelta) / stepDelta, 0.0, 1.0);
                    if (rel < 0.0) t = 0.0;
                    Sub s;
                    s.has = true;
                    s.button = static_cast<int>(in.m_button);
                    s.push = in.m_isPush;
                    s.p1 = !in.m_isPlayer2;
                    s.deltaFactor = std::clamp(t - elapsed, SMALLEST, 1.0);
                    s.endStep = false;
                    stepQueue.push_back(s);
                    elapsed = std::max(elapsed, t);
                    ++inputIdx;
                }
                else break;
            }
            Sub e;
            e.deltaFactor = std::max(SMALLEST, 1.0 - elapsed);
            e.endStep = true;
            stepQueue.push_back(e);
        }

        lastFrameTime = currentFrameTime;
        inputVector.erase(inputVector.begin(), inputVector.begin() + inputIdx);
    }

    // Раз в кадр из getModifiedDelta
    inline void onFrame(PlayLayer* pl, double modifiedDelta) {
        stepQueue.clear();

        if (mode == Mode::Play) {
            pl->m_queuedButtons.clear(); // реальный ввод при воспроизведении игнорируем
            return;
        }

        if (pl->m_playerDied) {
            skipUpdate = true;
            lastFrameTime = currentFrameTime;
            return;
        }
        // Кадр без физических шагов (при замедлении): клики остаются в очереди игры
        // и разложатся по шагам в следующем кадре со шагами.
        if (modifiedDelta <= 0.0) {
            skipUpdate = true;
            return;
        }

        float timewarp = pl->m_gameState.m_timeWarp;
        int stepCount = static_cast<int>(std::round(
            std::max(1.0, ((modifiedDelta * 60.0) / std::min(1.0f, timewarp)) * 4.0)
        ));
        buildStepQueue(pl, stepCount);
    }

    inline void onFrameStart() {
        currentFrameTime = getCurrentTimestamp();

        PlayLayer* pl = PlayLayer::get();
        CCNode* par = nullptr;
        bool idle = !pl
            || !(par = pl->getParent())
            || par->getChildByType<PauseLayer>(0)
            || pl->getChildByType<EndLevelLayer>(0);

        // замедляем только во время записи, когда игра реально идёт
        setTimeScale((mode == Mode::Record && !idle) ? static_cast<float>(recSpeed) : 1.0f);

        if (mode == Mode::Off) return;

        if (idle) {
            // пауза/конец уровня: держим якорь на текущем кадре, после возврата в игру первый кадр уже режется
            lastFrameTime = currentFrameTime;
            skipUpdate = true;
            inputVector.clear();
        }
    }

    inline void resetCollisionLog(PlayerObject* p) {
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
// Время кадра
// ---------------------------------------------------------------------------
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        bot::onFrameStart();
        CCScheduler::update(dt);
    }
};

// ---------------------------------------------------------------------------
// Ввод и очередь шагов
// ---------------------------------------------------------------------------
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        using namespace bot;
        if (mode == Mode::Off || silent > 0 || !isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (mode == Mode::Play) {
            if (applying > 0) GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return; // чужой ввод блокируем
        }

        // Record: пишем каждый применённый клик
        Action a;
        if (applying > 0) {
            a.step = applyStep;
            a.frac = applyFrac;
        }
        else {
            a.step = step; // клик пришёл обычным путём (до физики этого шага)
            a.frac = 0.0;
        }
        a.button = static_cast<uint8_t>(button);
        a.down = down;
        a.p1 = isPlayer1;
        macro.push_back(a);

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

    // На замедлении игра прогоняет шаг нулевой длины в каждом кадре без реальных шагов и обрабатывает в нём
    // очередь кликов. Прячем клики на время такого шага: они должны попасть в реальный шаг и разложиться по времени.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (bot::mode != bot::Mode::Off && bot::zeroFrame && bot::isPlayLayer(this)) {
            std::vector<PlayerButtonCommand> saved(this->m_queuedButtons.begin(), this->m_queuedButtons.end());
            this->m_queuedButtons.clear();
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            std::vector<PlayerButtonCommand> added(this->m_queuedButtons.begin(), this->m_queuedButtons.end());
            this->m_queuedButtons.clear();
            for (auto const& c : saved) this->m_queuedButtons.push_back(c);
            for (auto const& c : added) this->m_queuedButtons.push_back(c);
            return;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------------------
// Нарезка шага (схема Click Between Frames)
// ---------------------------------------------------------------------------
class $modify(BotPlayerObject, PlayerObject) {
    void update(float stepDelta) {
        using namespace bot;
        auto pl = PlayLayer::get();
        if (!pl || midStep) {
            PlayerObject::update(stepDelta);
            return;
        }

        // шаг нулевой длины: это не физический шаг, не считаем его и не режем
        if (zeroFrame || stepDelta <= 1e-9f) {
            PlayerObject::update(stepDelta);
            return;
        }

        if (mode == Mode::Off) {
            PlayerObject::update(stepDelta);
            if (this == pl->m_player1) ++step; // шаги считаем всегда: чекпоинты остаются валидными
            return;
        }

        // всю логику делаем в обновлении P1
        if (this != pl->m_player1) {
            if (!inputThisStep || this != pl->m_player2) PlayerObject::update(stepDelta);
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
        else { // Record
            bool front = !stepQueue.empty() && !stepQueue.front().endStep;
            if (!stepQueue.empty() && !front) stepQueue.pop_front();
            if (front) {
                double sum = 0.0;
                Sub s;
                do {
                    s = stepQueue.front();
                    stepQueue.pop_front();
                    subs.push_back(s);
                    sum += s.deltaFactor;
                } while (!stepQueue.empty() && !s.endStep);
                if (!subs.back().endStep) {
                    Sub e;
                    e.deltaFactor = std::max(SMALLEST, 1.0 - sum);
                    e.endStep = true;
                    subs.push_back(e);
                }
            }
        }

        inputThisStep = !subs.empty();

        if (subs.empty()) {
            p1Split = false;
            p2Split = false;
            PlayerObject::update(stepDelta);
            ++step;
            return;
        }

        PlayerObject* p2 = pl->m_player2;
        bool isDual = pl->m_gameState.m_isDualMode;
        bool p1StartedOnGround = this->m_isOnGround;
        bool p2StartedOnGround = p2->m_isOnGround;

        bool p1NotBuffering = p1StartedOnGround
            || this->m_touchingRings->count()
            || this->m_isDashing
            || (this->m_isDart || this->m_isBird || this->m_isShip || this->m_isSwing);

        bool p2NotBuffering = p2StartedOnGround
            || p2->m_touchingRings->count()
            || p2->m_isDashing
            || (p2->m_isDart || p2->m_isBird || p2->m_isShip || p2->m_isSwing);

        p1Pos = PlayerObject::getPosition();
        p2Pos = p2->getPosition();

        p1Split = p1NotBuffering;
        p2Split = p2NotBuffering && isDual;

        midStep = true;
        bool firstLoop = true;
        double acc = 0.0;
        double carry = 0.0;
        Sub pending;

        for (auto const& s : subs) {
            // клик, стоящий между предыдущей и этой частью
            if (pending.has) {
                double at = (p1Split || p2Split) ? acc : 0.0;
                applyInput(pl, pending, cur, at);
            }
            pending = s;

            // слишком короткую часть не считаем физикой (шум float), её время уходит в следующую часть
            double const part = s.deltaFactor + carry;
            if (!s.endStep && part < MIN_PART) {
                carry = part;
                acc += s.deltaFactor;
                continue;
            }
            carry = 0.0;
            float const substepDelta = static_cast<float>(stepDelta * part);
            rotationDelta = substepDelta;

            if (p1Split) {
                PlayerObject::update(substepDelta);
                if (!s.endStep) {
                    if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) this->m_isOnGround = p1StartedOnGround;
                    if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true);
                    else pl->checkCollisions(this, stepDelta, true);
                    PlayerObject::updateRotation(substepDelta);
                    resetCollisionLog(this);
                }
            }
            else if (s.endStep) PlayerObject::update(stepDelta);

            if (p2Split) {
                p2->update(substepDelta);
                if (!s.endStep) {
                    if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) p2->m_isOnGround = p2StartedOnGround;
                    if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
                    else pl->checkCollisions(p2, stepDelta, true);
                    p2->updateRotation(substepDelta);
                    resetCollisionLog(p2);
                }
            }
            else if (s.endStep) p2->update(stepDelta);

            acc += s.deltaFactor;
            firstLoop = false;
        }

        midStep = false;
        ++step;
    }

    // вызывается в конце обычного шага; доделываем вращение, оставшееся после нарезки
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
// Уровень, чекпоинты, рестарты
// ---------------------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        // при входе в уровень бот выключен, макрос сохраняется (можно загрузить и нажать PLAY)
        bot::mode = bot::Mode::Off;
        bot::step = 0;
        bot::playIndex = 0;
        bot::checkpoints.clear();
        bot::resetEngine();
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
        if (bot::cpLoaded) return; // откат на чекпоинт обработан в loadFromCheckpoint

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

        auto it = bot::checkpoints.find(cp);
        if (it == bot::checkpoints.end()) {
            // неизвестный чекпоинт: синхронизироваться нельзя, безопаснее выключить бота
            bot::step = 0;
            bot::playIndex = 0;
            if (bot::mode != bot::Mode::Off) {
                bot::setMode(bot::Mode::Off);
                Notification::create("Bot: unknown checkpoint, OFF", NotificationIcon::Warning)->show();
            }
            return;
        }

        bot::step = it->second;

        if (bot::mode == bot::Mode::Record) {
            uint32_t s = bot::step;
            auto& m = bot::macro;
            m.erase(
                std::remove_if(m.begin(), m.end(), [s](bot::Action const& a) { return a.step >= s; }),
                m.end()
            );
        }
        else if (bot::mode == bot::Mode::Play) {
            bot::syncPlayback(this);
        }
    }
};

// ---------------------------------------------------------------------------
// Панель в меню паузы
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
            this->onRestart(nullptr); // рестарт с начала и выход из паузы (this дальше не используем)
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
