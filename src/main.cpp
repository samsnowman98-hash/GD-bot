// Sub-step (time-based) macro bot for Geometry Dash 2.2081 / Geode 5.x (Android)
//
// Идея (по мотивам Click Between Frames):
//  * Время клика = (номер физического шага) + (доля шага 0..1). Не зависит от FPS.
//  * Запись: реальные нажатия (m_queuedButtons с timestamp) раскладываются по шагам кадра,
//    шаг режется на части, между частями вызывается handleButton. Каждому применённому
//    клику сохраняем {step, frac}.
//  * Воспроизведение: для текущего шага берём клики из макроса и режем шаг так же.
//  * Режим OFF полностью пассивный (ванильная игра).
//  * Физика с неполными шагами НЕ совпадает с ванильной, макрос работает только с этим модом.
//  * Не совместим с Click Between Frames и родными Click Between/On Steps (мы их отключаем).

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t step;   // номер физического шага от начала попытки
        double frac;     // доля шага 0..1
        uint8_t button;
        bool down;
        bool p1;
    };

    // Одна часть разрезанного шага
    struct Sub {
        bool has = false;       // есть ли клик, применяемый ПОСЛЕ этой части
        int button = 1;
        bool push = false;
        bool p1 = true;
        double deltaFactor = 1.0;
        bool endStep = true;
    };

    struct State {
        Mode mode = Mode::Off;
        uint32_t step = 0;      // номер текущего шага (считаем по обновлениям P1)
        size_t index = 0;       // следующее действие для воспроизведения
        int applying = 0;       // >0 пока мы сами вызываем handleButton
        uint32_t applyStep = 0;
        double applyFrac = 0.0;
        bool cpLoaded = false;
        int levelID = 0;
        std::vector<Action> macro;
        std::unordered_map<CheckpointObject*, uint32_t> checkpoints;
    };

    inline State g;

    // ---- состояние движка нарезки шагов ----
    inline std::deque<Sub> stepQueue;
    inline std::vector<PlayerButtonCommand> inputVector;
    inline double lastFrameTime = 0.0;
    inline double currentFrameTime = 0.0;
    inline bool firstFrame = true;
    inline bool skipUpdate = true;
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
        firstFrame = true;
        skipUpdate = true;
        inputThisStep = false;
        p1Split = false;
        p2Split = false;
        midStep = false;
    }

    // Отключаем родные Click Between/On Steps пока бот активен, иначе возвращаем настройки игрока
    inline void applyClickFlags(PlayLayer* pl) {
        if (!pl) return;
        if (g.mode != Mode::Off) {
            pl->m_clickBetweenSteps = false;
            pl->m_clickOnSteps = false;
        }
        else {
            auto gm = GameManager::get();
            pl->m_clickBetweenSteps = gm->getGameVariable("0177");
            pl->m_clickOnSteps = gm->getGameVariable("0176");
        }
    }

    inline char const* modeName() {
        switch (g.mode) {
            case Mode::Record: return "REC";
            case Mode::Play: return "PLAY";
            default: return "OFF";
        }
    }

    inline std::string statusText() {
        return fmt::format("Bot: {} | actions: {}", modeName(), g.macro.size());
    }

    inline void notify(char const* text, NotificationIcon icon = NotificationIcon::Info) {
        Notification::create(text, icon, 1.f)->show();
    }

    // ---- файл макроса ----
    inline std::filesystem::path macroPath() {
        return Mod::get()->getSaveDir() / fmt::format("level_{}.macro", g.levelID);
    }

    constexpr uint32_t kMagic = 0x32544F42; // "BOT2"

    inline void saveMacro() {
        std::ofstream f(macroPath(), std::ios::binary | std::ios::trunc);
        if (!f) {
            log::error("Cannot open macro file for writing");
            return;
        }
        uint32_t magic = kMagic;
        uint32_t count = static_cast<uint32_t>(g.macro.size());
        f.write(reinterpret_cast<char const*>(&magic), sizeof(magic));
        f.write(reinterpret_cast<char const*>(&count), sizeof(count));
        for (auto const& a : g.macro) {
            uint8_t flags = static_cast<uint8_t>((a.down ? 1 : 0) | (a.p1 ? 2 : 0));
            f.write(reinterpret_cast<char const*>(&a.step), sizeof(a.step));
            f.write(reinterpret_cast<char const*>(&a.frac), sizeof(a.frac));
            f.write(reinterpret_cast<char const*>(&flags), sizeof(flags));
            f.write(reinterpret_cast<char const*>(&a.button), sizeof(a.button));
        }
    }

    inline void loadMacro() {
        g.macro.clear();
        std::ifstream f(macroPath(), std::ios::binary);
        if (!f) return;
        uint32_t magic = 0, count = 0;
        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        f.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!f || magic != kMagic || count > 5'000'000) return;
        std::vector<Action> tmp;
        tmp.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Action a{};
            uint8_t flags = 0;
            f.read(reinterpret_cast<char*>(&a.step), sizeof(a.step));
            f.read(reinterpret_cast<char*>(&a.frac), sizeof(a.frac));
            f.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            f.read(reinterpret_cast<char*>(&a.button), sizeof(a.button));
            if (!f) return; // битый файл - не грузим
            a.down = flags & 1;
            a.p1 = flags & 2;
            tmp.push_back(a);
        }
        g.macro = std::move(tmp);
    }

    // ---- применение клика (с отметкой времени для записи) ----
    inline void applyInput(PlayLayer* pl, Sub const& s, uint32_t step, double frac) {
        ++g.applying;
        g.applyStep = step;
        g.applyFrac = std::clamp(frac, 0.0, 1.0);
        pl->handleButton(s.push, s.button, s.p1);
        --g.applying;
    }

    // Восстановить зажатые кнопки на g.step и выставить индекс воспроизведения
    inline void syncPlayback(PlayLayer* pl) {
        auto it = std::lower_bound(
            g.macro.begin(), g.macro.end(), g.step,
            [](Action const& a, uint32_t s) { return a.step < s; }
        );
        g.index = static_cast<size_t>(it - g.macro.begin());

        bool held[4][2] = {};
        for (size_t i = 0; i < g.index; ++i) {
            auto const& a = g.macro[i];
            if (a.button < 4) held[a.button][a.p1 ? 0 : 1] = a.down;
        }
        ++g.applying;
        for (int b = 1; b <= 3; ++b) {
            pl->handleButton(held[b][0], b, true);
            pl->handleButton(held[b][1], b, false);
        }
        --g.applying;
    }

    /*
    Раскладывает реальные клики кадра по физическим шагам кадра (как в CBF).
    Для каждого шага: [часть до клика] [клик] [часть до след. клика] ... [остаток шага]
    */
    inline void buildStepQueue(PlayLayer* pl, int stepCount) {
        stepQueue.clear();
        skipUpdate = false;

        if (firstFrame) {
            skipUpdate = true;
            firstFrame = false;
            lastFrameTime = currentFrameTime;
            inputVector.clear();
            return;
        }

        double deltaTime = currentFrameTime - lastFrameTime;
        if (!(deltaTime > 0.0) || stepCount < 1) {
            skipUpdate = true;
            lastFrameTime = currentFrameTime;
            return;
        }

        inputVector.insert(inputVector.end(), pl->m_queuedButtons.begin(), pl->m_queuedButtons.end());
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

    // Вызывается раз в кадр из getModifiedDelta
    inline void onFrame(PlayLayer* pl, double modifiedDelta) {
        stepQueue.clear();

        if (g.mode == Mode::Play) {
            pl->m_queuedButtons.clear(); // реальный ввод во время воспроизведения игнорируем
            return;
        }

        if (pl->m_playerDied) {
            skipUpdate = true;
            firstFrame = true;
            return;
        }
        // Кадр без физических шагов (бывает при замедлении): ничего не сбрасываем,
        // клики накопятся и разложатся по шагам в следующем кадре со шагами.
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

    // Начало кадра: время кадра и сброс при паузе/выходе из уровня
    // Скорость игры при записи (настройка "record-speed", 0.01..1.0)
    inline double recSpeed = 1.0;
    inline float appliedScale = 1.0f;

    inline void setTimeScale(float s) {
        if (std::fabs(s - appliedScale) < 1e-6f) return;
        appliedScale = s;
        CCDirector::get()->getScheduler()->setTimeScale(s);
    }

    inline void onFrameStart() {
        currentFrameTime = getCurrentTimestamp();

        PlayLayer* pl = PlayLayer::get();
        CCNode* par = nullptr;
        bool idle = !pl
            || !(par = pl->getParent())
            || par->getChildByType<PauseLayer>(0)
            || pl->getChildByType<EndLevelLayer>(0);

        // замедляем только пока идёт запись и игра реально идёт (не пауза/выход)
        setTimeScale((g.mode == Mode::Record && !idle) ? static_cast<float>(recSpeed) : 1.0f);

        if (g.mode == Mode::Off) return;

        if (idle) {
            firstFrame = true;
            skipUpdate = true;
            inputVector.clear();
        }
    }

    // PlayerObject::resetCollisionLog() может быть заинлайнен, пишем вручную
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
// Ввод
// ---------------------------------------------------------------------------
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& g = bot::g;
        if (g.mode == bot::Mode::Off || !bot::isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (g.mode == bot::Mode::Play) {
            if (g.applying <= 0) return; // чужой ввод блокируем
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Record: пишем каждый применённый клик
        bot::Action a{};
        if (g.applying > 0) {
            a.step = g.applyStep;
            a.frac = g.applyFrac;
        }
        else {
            a.step = g.step; // клик, пришедший обычным путём (до физики этого шага)
            a.frac = 0.0;
        }
        a.button = static_cast<uint8_t>(button);
        a.down = down;
        a.p1 = isPlayer1;
        g.macro.push_back(a);

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    double getModifiedDelta(float dt) {
        double md = GJBaseGameLayer::getModifiedDelta(dt);
        if (bot::g.mode != bot::Mode::Off && bot::isPlayLayer(this)) {
            bot::onFrame(PlayLayer::get(), md);
        }
        return md;
    }
};

// ---------------------------------------------------------------------------
// Нарезка шага (на основе Click Between Frames)
// ---------------------------------------------------------------------------
class $modify(BotPlayerObject, PlayerObject) {
    void update(float stepDelta) {
        auto pl = PlayLayer::get();
        if (!pl || bot::g.mode == bot::Mode::Off || bot::midStep) {
            PlayerObject::update(stepDelta);
            return;
        }

        // всю логику делаем в обновлении P1
        if (this != pl->m_player1) {
            if (!bot::inputThisStep || this != pl->m_player2) PlayerObject::update(stepDelta);
            return;
        }

        auto& g = bot::g;
        uint32_t const cur = g.step;
        std::vector<bot::Sub> subs;

        if (g.mode == bot::Mode::Play) {
            double elapsed = 0.0;
            while (g.index < g.macro.size() && g.macro[g.index].step <= cur) {
                auto const& a = g.macro[g.index++];
                double f = (a.step < cur) ? 0.0 : std::clamp(a.frac, 0.0, 1.0);
                bot::Sub s;
                s.has = true;
                s.button = a.button;
                s.push = a.down;
                s.p1 = a.p1;
                s.deltaFactor = std::clamp(f - elapsed, bot::SMALLEST, 1.0);
                s.endStep = false;
                subs.push_back(s);
                elapsed = std::max(elapsed, f);
            }
            if (!subs.empty()) {
                bot::Sub e;
                e.deltaFactor = std::max(bot::SMALLEST, 1.0 - elapsed);
                e.endStep = true;
                subs.push_back(e);
            }
        }
        else {
            bool front = !bot::stepQueue.empty() && !bot::stepQueue.front().endStep;
            if (!bot::stepQueue.empty() && !front) bot::stepQueue.pop_front();
            if (front && !bot::skipUpdate) {
                double sum = 0.0;
                bot::Sub s;
                do {
                    s = bot::stepQueue.front();
                    bot::stepQueue.pop_front();
                    subs.push_back(s);
                    sum += s.deltaFactor;
                } while (!bot::stepQueue.empty() && !s.endStep);
                if (!subs.back().endStep) {
                    bot::Sub e;
                    e.deltaFactor = std::max(bot::SMALLEST, 1.0 - sum);
                    e.endStep = true;
                    subs.push_back(e);
                }
            }
        }

        bot::inputThisStep = !subs.empty();

        if (subs.empty()) {
            bot::p1Split = false;
            bot::p2Split = false;
            PlayerObject::update(stepDelta);
            ++g.step;
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

        bot::p1Pos = PlayerObject::getPosition();
        bot::p2Pos = p2->getPosition();

        bot::p1Split = p1NotBuffering;
        bot::p2Split = p2NotBuffering && isDual;

        bot::midStep = true;
        bool firstLoop = true;
        double acc = 0.0;
        bot::Sub pending;

        for (auto const& s : subs) {
            // клик, стоящий между предыдущей и этой частью
            if (pending.has) {
                double at = (bot::p1Split || bot::p2Split) ? acc : 0.0;
                bot::applyInput(pl, pending, cur, at);
            }
            pending = s;

            float const substepDelta = static_cast<float>(stepDelta * s.deltaFactor);
            bot::rotationDelta = substepDelta;

            if (bot::p1Split) {
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

            if (bot::p2Split) {
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

            acc += s.deltaFactor;
            firstLoop = false;
        }

        bot::midStep = false;
        ++g.step;
    }

    // вызывается в конце обычного шага; доделываем вращение, оставшееся после нарезки
    void updateRotation(float t) {
        auto pl = PlayLayer::get();
        if (pl && bot::g.mode != bot::Mode::Off && !bot::midStep) {
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
        bot::g = bot::State{};
        bot::resetEngine();
        if (level) bot::g.levelID = level->m_levelID.value();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        bot::loadMacro();
        return true;
    }

    void resetLevel() {
        bot::g.cpLoaded = false;
        PlayLayer::resetLevel();
        bot::resetEngine();
        if (bot::g.cpLoaded) return; // откат на чекпоинт обработан в loadFromCheckpoint

        bot::g.step = 0;
        bot::g.index = 0;
        bot::g.checkpoints.clear();
        if (bot::g.mode == bot::Mode::Record) bot::g.macro.clear();
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (cp) bot::g.checkpoints[cp] = bot::g.step;
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        bot::g.cpLoaded = true;
        bot::resetEngine();

        auto it = bot::g.checkpoints.find(cp);
        if (it == bot::g.checkpoints.end()) {
            bot::g.step = 0;
            bot::g.index = 0;
            if (bot::g.mode != bot::Mode::Off) {
                bot::g.mode = bot::Mode::Off;
                bot::applyClickFlags(this);
                bot::notify("Bot: unknown checkpoint, OFF", NotificationIcon::Warning);
            }
            return;
        }

        bot::g.step = it->second;

        if (bot::g.mode == bot::Mode::Record) {
            auto& m = bot::g.macro;
            m.erase(
                std::remove_if(m.begin(), m.end(), [](bot::Action const& a) {
                    return a.step >= bot::g.step;
                }),
                m.end()
            );
        }
        else if (bot::g.mode == bot::Mode::Play) {
            bot::syncPlayback(this);
        }
    }

    void levelComplete() {
        if (bot::g.mode == bot::Mode::Record) {
            if (!bot::g.macro.empty()) {
                bot::saveMacro();
                bot::notify("Macro saved!", NotificationIcon::Success);
            }
            bot::g.mode = bot::Mode::Off;
            bot::applyClickFlags(this);
            bot::resetEngine();
        }
        PlayLayer::levelComplete();
    }
};

// ---------------------------------------------------------------------------
// Кнопки в меню паузы
// ---------------------------------------------------------------------------
class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!PlayLayer::get()) return;

        auto win = CCDirector::get()->getWinSize();

        auto menu = CCMenu::create();
        menu->setID("bot-menu");
        menu->setPosition({ 0.f, 0.f });

        auto label = CCLabelBMFont::create(bot::statusText().c_str(), "chatFont.fnt");
        label->setID("bot-status");
        label->setScale(0.6f);
        label->setAnchorPoint({ 0.f, 0.5f });
        label->setPosition({ 10.f, win.height / 2.f + 75.f });
        this->addChild(label);

        float x = 45.f;
        float y = win.height / 2.f + 45.f;

        auto add = [&](char const* text, char const* id, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(text);
            spr->setScale(0.6f);
            auto item = CCMenuItemSpriteExtra::create(spr, this, cb);
            item->setID(id);
            item->setPosition({ x, y });
            menu->addChild(item);
            y -= 30.f;
        };

        add("REC",   "bot-rec",   menu_selector(BotPauseLayer::onBotRec));
        add("PLAY",  "bot-play",  menu_selector(BotPauseLayer::onBotPlay));
        add("STOP",  "bot-stop",  menu_selector(BotPauseLayer::onBotStop));
        add("CLEAR", "bot-clear", menu_selector(BotPauseLayer::onBotClear));

        this->addChild(menu);
    }

    void updateBotLabel() {
        if (auto l = typeinfo_cast<CCLabelBMFont*>(this->getChildByID("bot-status"))) {
            l->setString(bot::statusText().c_str());
        }
    }

    void onBotRec(CCObject*) {
        bot::g.macro.clear();
        bot::g.mode = bot::Mode::Record;
        bot::applyClickFlags(PlayLayer::get());
        bot::resetEngine();
        bot::notify("Recording...", NotificationIcon::Info);
        this->onRestart(nullptr); // рестарт с начала и выход из паузы
    }

    void onBotPlay(CCObject*) {
        if (bot::g.macro.empty()) {
            bot::notify("No macro!", NotificationIcon::Warning);
            return;
        }
        bot::g.mode = bot::Mode::Play;
        bot::applyClickFlags(PlayLayer::get());
        bot::resetEngine();
        bot::notify("Playing...", NotificationIcon::Success);
        this->onRestart(nullptr);
    }

    void onBotStop(CCObject*) {
        if (bot::g.mode == bot::Mode::Record && !bot::g.macro.empty()) {
            bot::saveMacro();
            bot::notify("Macro saved!", NotificationIcon::Success);
        }
        bot::g.mode = bot::Mode::Off;
        bot::applyClickFlags(PlayLayer::get());
        bot::resetEngine();
        this->updateBotLabel();
    }

    void onBotClear(CCObject*) {
        bot::g.macro.clear();
        bot::g.mode = bot::Mode::Off;
        bot::applyClickFlags(PlayLayer::get());
        bot::resetEngine();
        std::error_code ec;
        std::filesystem::remove(bot::macroPath(), ec);
        bot::notify("Macro cleared", NotificationIcon::Success);
        this->updateBotLabel();
    }
};

$on_mod(Loaded) {
    bot::recSpeed = std::clamp(Mod::get()->getSettingValue<double>("record-speed"), 0.01, 1.0);
    listenForSettingChanges<double>("record-speed", +[](double v) {
        bot::recSpeed = std::clamp(v, 0.01, 1.0);
    });
}
