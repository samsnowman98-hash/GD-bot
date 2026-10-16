// Sub-step (time-based) macro bot for Geometry Dash 2.2081 / Geode 5.x
// Cross-platform: Android (ARM64) & Windows (x64)

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace bot {

    constexpr double SMALLEST = 0.000001;
    constexpr char const* kMacroExt = ".gdbot";
    constexpr uint32_t kMagic = 0x32544F42; // "BOT2"

    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t step;   // физический шаг (240 TPS)
        double frac;     // точная доля шага 0.0 .. 1.0 (< 1 мс)
        uint8_t button;
        bool down;
        bool p1;
    };

    struct Sub {
        bool has = false;
        int button = 1;
        bool push = false;
        bool p1 = true;
        double deltaFactor = 1.0;
        bool endStep = true;
    };

    // Точные физические параметры для идеального респавна на чекпоинте
    struct CheckpointData {
        uint32_t step = 0;
        CCPoint p1Pos = { 0.f, 0.f };
        double p1YVel = 0.0;
        float p1Rot = 0.f;
        CCPoint p2Pos = { 0.f, 0.f };
        double p2YVel = 0.0;
        float p2Rot = 0.f;
    };

    struct State {
        Mode mode = Mode::Off;
        uint32_t step = 0;
        size_t index = 0;
        int applying = 0;
        uint32_t applyStep = 0;
        double applyFrac = 0.0;
        bool cpLoaded = false;
        int levelID = 0;
        std::string levelName = "level";
        uint64_t levelHash = 0;
        std::vector<Action> macro;
        std::unordered_map<CheckpointObject*, CheckpointData> checkpoints;
    };

    inline State g;

    // ---- Движок нарезки шагов ----
    inline std::deque<Sub> stepQueue;
    inline std::vector<PlayerButtonCommand> inputVector;
    inline double lastFrameTime = 0.0;
    inline double currentFrameTime = 0.0;
    inline bool firstFrame = true;
    inline bool skipUpdate = false;
    inline bool inputThisStep = false;
    inline bool p1Split = false;
    inline bool p2Split = false;
    inline bool midStep = false;
    inline CCPoint p1Pos = { 0.f, 0.f };
    inline CCPoint p2Pos = { 0.f, 0.f };
    inline float rotationDelta = 0.f;

    // ---- Скорость записи ----
    inline double recSpeed = 1.0;
    inline float appliedScale = 1.0f;

    // Кроссплатформенный таймер наносекундной точности
    inline double getCurrentTimestamp() {
        using namespace std::chrono;
        return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
    }

    inline bool isPlayLayer(GJBaseGameLayer* l) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == l;
    }

    // Мгновенная инициализация физики на 1-м кадре после респавна
    inline void primeEngineAfterRespawn(double expectedDelta) {
        stepQueue.clear();
        inputVector.clear();
        currentFrameTime = getCurrentTimestamp();
        double factor = (g.mode == Mode::Record) ? recSpeed : 1.0;
        lastFrameTime = currentFrameTime - (expectedDelta * factor);
        firstFrame = false;
        skipUpdate = false;
        inputThisStep = false;
        p1Split = false;
        p2Split = false;
        midStep = false;
    }

    inline void resetEngine() {
        stepQueue.clear();
        inputVector.clear();
        firstFrame = true;
        skipUpdate = false;
        inputThisStep = false;
        p1Split = false;
        p2Split = false;
        midStep = false;
    }

    inline void applyClickFlags(PlayLayer* pl) {
        if (!pl) return;
        if (g.mode != Mode::Off) {
            pl->m_clickBetweenSteps = true; // Сбор высокоточных аппаратных нажатий
            pl->m_clickOnSteps = false;
        } else {
            auto gm = GameManager::get();
            pl->m_clickBetweenSteps = gm->getGameVariable("0177");
            pl->m_clickOnSteps = gm->getGameVariable("0176");
        }
    }

    inline char const* modeName() {
        switch (g.mode) {
            case Mode::Record: return "REC";
            case Mode::Play:   return "PLAY";
            default:           return "OFF";
        }
    }

    inline std::string statusText() {
        return fmt::format("Bot: {} | acts: {} | Spd: {:.2f}x", modeName(), g.macro.size(), recSpeed);
    }

    inline void notify(char const* text, NotificationIcon icon = NotificationIcon::Info) {
        Notification::create(text, icon, 1.f)->show();
    }

    // ---- Файловая система (.gdbot) ----
    inline std::filesystem::path macroPath() {
        if (g.levelID != 0) {
            return Mod::get()->getSaveDir() / fmt::format("level_{}{}", g.levelID, kMacroExt);
        }
        return Mod::get()->getSaveDir() / fmt::format("local_{}{}", g.levelHash, kMacroExt);
    }

    inline bool writeMacroToFile(std::filesystem::path const& path, std::vector<Action> const& macro) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        uint32_t magic = kMagic;
        uint32_t count = static_cast<uint32_t>(macro.size());
        f.write(reinterpret_cast<char const*>(&magic), sizeof(magic));
        f.write(reinterpret_cast<char const*>(&count), sizeof(count));
        for (auto const& a : macro) {
            uint8_t flags = static_cast<uint8_t>((a.down ? 1 : 0) | (a.p1 ? 2 : 0));
            f.write(reinterpret_cast<char const*>(&a.step), sizeof(a.step));
            f.write(reinterpret_cast<char const*>(&a.frac), sizeof(a.frac));
            f.write(reinterpret_cast<char const*>(&flags), sizeof(flags));
            f.write(reinterpret_cast<char const*>(&a.button), sizeof(a.button));
        }
        return true;
    }

    inline bool readMacroFromFile(std::filesystem::path const& path, std::vector<Action>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        uint32_t magic = 0, count = 0;
        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        f.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!f || magic != kMagic || count > 5'000'000) return false;
        std::vector<Action> tmp;
        tmp.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Action a{};
            uint8_t flags = 0;
            f.read(reinterpret_cast<char*>(&a.step), sizeof(a.step));
            f.read(reinterpret_cast<char*>(&a.frac), sizeof(a.frac));
            f.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            f.read(reinterpret_cast<char*>(&a.button), sizeof(a.button));
            if (!f) return false;
            a.down = flags & 1;
            a.p1 = flags & 2;
            tmp.push_back(a);
        }
        out = std::move(tmp);
        return true;
    }

    inline void saveMacro() {
        if (!writeMacroToFile(macroPath(), g.macro)) {
            log::error("Cannot open macro file for writing");
        }
    }

    inline void loadMacro() {
        g.macro.clear();
        std::vector<Action> tmp;
        if (readMacroFromFile(macroPath(), tmp)) {
            g.macro = std::move(tmp);
        }
    }

    inline void applyInput(PlayLayer* pl, Sub const& s, uint32_t step, double frac) {
        ++g.applying;
        g.applyStep = step;
        g.applyFrac = std::clamp(frac, 0.0, 1.0);
        pl->handleButton(s.push, s.button, s.p1);
        --g.applying;
    }

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

    // Нарезка шагов с защитой от лагов (Lag Shield)
    inline void buildStepQueue(PlayLayer* pl, int stepCount) {
        double timeScaleFactor = (g.mode == Mode::Record) ? recSpeed : 1.0;
        double deltaTime = (currentFrameTime - lastFrameTime) * timeScaleFactor;

        // Защита от резких лагов системы (максимум 40 мс)
        double maxAllowedDelta = (1.0 / 25.0) * timeScaleFactor;
        if (deltaTime > maxAllowedDelta) {
            deltaTime = maxAllowedDelta;
        }

        if (!(deltaTime > 0.0) || stepCount < 1) {
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
                double rel = (in.m_timestamp - lastFrameTime) * timeScaleFactor;
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
                } else break;
            }
            Sub e;
            e.deltaFactor = std::max(SMALLEST, 1.0 - elapsed);
            e.endStep = true;
            stepQueue.push_back(e);
        }

        lastFrameTime = currentFrameTime;
        inputVector.erase(inputVector.begin(), inputVector.begin() + inputIdx);
    }

    inline void onFrame(PlayLayer* pl, double modifiedDelta) {
        if (g.mode == Mode::Play) {
            pl->m_queuedButtons.clear();
            return;
        }
        if (pl->m_playerDied) {
            stepQueue.clear();
            firstFrame = true;
            return;
        }
        if (modifiedDelta <= 0.0) return;

        // При возрождении не пропускаем кадр, а сразу инициализируем время
        if (firstFrame) {
            primeEngineAfterRespawn(modifiedDelta);
        }

        float timewarp = pl->m_gameState.m_timeWarp;
        int stepCount = static_cast<int>(std::round(
            std::max(1.0, ((modifiedDelta * 60.0) / std::min(1.0f, timewarp)) * 4.0)
        ));
        buildStepQueue(pl, stepCount);
    }

    inline void setTimeScale(float s) {
        if (std::fabs(s - appliedScale) < 1e-5f) return;
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

        setTimeScale((g.mode == Mode::Record && !idle) ? static_cast<float>(recSpeed) : 1.0f);

        if (g.mode == Mode::Off) return;
        if (idle) {
            firstFrame = true;
            inputVector.clear();
        }
    }

    inline void resetCollisionLog(PlayerObject* p) {
        if (!p) return;
        if (p->m_collisionLogTop) p->m_collisionLogTop->removeAllObjects();
        if (p->m_collisionLogBottom) p->m_collisionLogBottom->removeAllObjects();
        if (p->m_collisionLogLeft) p->m_collisionLogLeft->removeAllObjects();
        if (p->m_collisionLogRight) p->m_collisionLogRight->removeAllObjects();
        p->m_lastCollisionLeft = -1;
        p->m_lastCollisionRight = -1;
        p->m_lastCollisionBottom = -1;
        p->m_lastCollisionTop = -1;
    }
}

// ---------------------------------------------------------------------------
// МЕНЮ МАКРОСОВ (Macro Manager Popup)
// ---------------------------------------------------------------------------
class MacroManagerPopup : public geode::Popup<> {
protected:
    ScrollLayer* m_scroll = nullptr;
    CCMenu* m_listMenu = nullptr;

    bool setup() override {
        this->setTitle("Macro Manager");

        auto winSize = m_mainLayer->getContentSize();

        m_scroll = ScrollLayer::create({ 310.f, 130.f });
        m_scroll->setPosition({ (winSize.width - 310.f) / 2.f, 50.f });
        m_mainLayer->addChild(m_scroll);

        auto btmMenu = CCMenu::create();
        btmMenu->setPosition({ winSize.width / 2.f, 25.f });

        auto saveSpr = ButtonSprite::create("SAVE CURRENT", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        saveSpr->setScale(0.6f);
        auto saveBtn = CCMenuItemSpriteExtra::create(saveSpr, this, menu_selector(MacroManagerPopup::onSaveNew));
        btmMenu->addChild(saveBtn);
        m_mainLayer->addChild(btmMenu);

        this->loadFileList();
        return true;
    }

    void loadFileList() {
        if (m_listMenu) m_listMenu->removeFromParent();

        std::vector<std::filesystem::path> files;
        auto dir = Mod::get()->getSaveDir();
        if (std::filesystem::exists(dir)) {
            for (auto const& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.path().extension() == bot::kMacroExt) {
                    files.push_back(entry.path());
                }
            }
        }

        std::sort(files.begin(), files.end());

        float rowHeight = 28.f;
        float totalHeight = std::max(130.f, static_cast<float>(files.size()) * rowHeight);

        m_scroll->m_contentLayer->setContentSize({ 310.f, totalHeight });

        m_listMenu = CCMenu::create();
        m_listMenu->setPosition({ 0.f, 0.f });
        m_scroll->m_contentLayer->addChild(m_listMenu);

        if (files.empty()) {
            auto emptyLabel = CCLabelBMFont::create("No saved macros yet", "chatFont.fnt");
            emptyLabel->setScale(0.6f);
            emptyLabel->setPosition({ 155.f, totalHeight / 2.f });
            m_scroll->m_contentLayer->addChild(emptyLabel);
            return;
        }

        for (size_t i = 0; i < files.size(); ++i) {
            auto const& path = files[i];
            float y = totalHeight - (static_cast<float>(i) * rowHeight) - (rowHeight / 2.f);

            std::string nameStr = path.stem().string();
            if (nameStr.length() > 22) {
                nameStr = nameStr.substr(0, 20) + "..";
            }

            auto nameLabel = CCLabelBMFont::create(nameStr.c_str(), "chatFont.fnt");
            nameLabel->setScale(0.48f);
            nameLabel->setAnchorPoint({ 0.f, 0.5f });
            nameLabel->setPosition({ 10.f, y });
            m_listMenu->addChild(nameLabel);

            // Кнопка LOAD
            auto loadSpr = ButtonSprite::create("LOAD");
            loadSpr->setScale(0.42f);
            auto loadBtn = CCMenuItemSpriteExtra::create(loadSpr, this, menu_selector(MacroManagerPopup::onLoadFile));
            loadBtn->setUserObject(CCString::create(path.string()));
            loadBtn->setPosition({ 220.f, y });
            m_listMenu->addChild(loadBtn);

            // Кнопка DELETE
            auto delSpr = ButtonSprite::create("X", "bigFont.fnt", "GJ_button_06.png", 0.6f);
            delSpr->setScale(0.42f);
            auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(MacroManagerPopup::onDeleteFile));
            delBtn->setUserObject(CCString::create(path.string()));
            delBtn->setPosition({ 275.f, y });
            m_listMenu->addChild(delBtn);
        }

        m_scroll->moveToTop();
    }

    void onSaveNew(CCObject*) {
        if (bot::g.macro.empty()) {
            bot::notify("Macro is empty!", NotificationIcon::Warning);
            return;
        }
        auto now = std::time(nullptr);
        auto ltm = std::localtime(&now);
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%m%d_%H%M%S", ltm);

        std::string filename = fmt::format("{}_{}{}", bot::g.levelName, timeBuf, bot::kMacroExt);
        auto target = Mod::get()->getSaveDir() / filename;

        if (bot::writeMacroToFile(target, bot::g.macro)) {
            bot::notify("Saved successfully!", NotificationIcon::Success);
            this->loadFileList();
        } else {
            bot::notify("Failed to save!", NotificationIcon::Error);
        }
    }

    void onLoadFile(CCObject* sender) {
        auto item = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto strObj = static_cast<CCString*>(item->getUserObject());
        std::filesystem::path p(strObj->getCString());

        std::vector<bot::Action> tmp;
        if (bot::readMacroFromFile(p, tmp)) {
            bot::g.macro = std::move(tmp);
            bot::g.index = 0;
            bot::notify(fmt::format("Loaded: {} actions", bot::g.macro.size()).c_str(), NotificationIcon::Success);
            this->onClose(nullptr);
        } else {
            bot::notify("Corrupted file!", NotificationIcon::Error);
        }
    }

    void onDeleteFile(CCObject* sender) {
        auto item = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto strObj = static_cast<CCString*>(item->getUserObject());
        std::error_code ec;
        std::filesystem::remove(strObj->getCString(), ec);
        bot::notify("Macro deleted", NotificationIcon::Info);
        this->loadFileList();
    }

public:
    static MacroManagerPopup* create() {
        auto ret = new MacroManagerPopup();
        if (ret && ret->initAnchored(350.f, 210.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// ХУКИ
// ---------------------------------------------------------------------------

class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        bot::onFrameStart();
        CCScheduler::update(dt);
    }
};

class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& g = bot::g;
        if (g.mode == bot::Mode::Off || !bot::isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (g.mode == bot::Mode::Play) {
            if (g.applying <= 0) return;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        bot::Action a{};
        if (g.applying > 0) {
            a.step = g.applyStep;
            a.frac = g.applyFrac;
        } else {
            a.step = g.step;
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

class $modify(BotPlayerObject, PlayerObject) {
    void update(float stepDelta) {
        auto pl = PlayLayer::get();
        if (!pl || bot::g.mode == bot::Mode::Off || bot::midStep) {
            PlayerObject::update(stepDelta);
            return;
        }

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
        } else {
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
        bool p2StartedOnGround = p2 ? p2->m_isOnGround : false;

        bool p1Rings = this->m_touchingRings && (this->m_touchingRings->count() > 0);
        bool p1NotBuffering = p1StartedOnGround
            || p1Rings
            || this->m_isDashing
            || (this->m_isDart || this->m_isBird || this->m_isShip || this->m_isSwing);

        bool p2Rings = p2 && p2->m_touchingRings && (p2->m_touchingRings->count() > 0);
        bool p2NotBuffering = p2 && (p2StartedOnGround
            || p2Rings
            || p2->m_isDashing
            || (p2->m_isDart || p2->m_isBird || p2->m_isShip || p2->m_isSwing));

        bot::p1Pos = PlayerObject::getPosition();
        if (p2) bot::p2Pos = p2->getPosition();

        bot::p1Split = p1NotBuffering;
        bot::p2Split = p2NotBuffering && isDual;

        bot::midStep = true;
        bool firstLoop = true;
        double acc = 0.0;

        // Ликвидация погрешности float: точный учет уже пройденных долей шага
        float consumedDeltaP1 = 0.0f;
        float consumedDeltaP2 = 0.0f;
        bot::Sub pending;

        for (size_t i = 0; i < subs.size(); ++i) {
            auto const& s = subs[i];

            if (pending.has) {
                double at = (bot::p1Split || bot::p2Split) ? acc : 0.0;
                bot::applyInput(pl, pending, cur, at);
            }
            pending = s;

            float const substepDeltaP1 = s.endStep ? std::max(0.0f, stepDelta - consumedDeltaP1)
                                                   : static_cast<float>(stepDelta * s.deltaFactor);
            float const substepDeltaP2 = s.endStep ? std::max(0.0f, stepDelta - consumedDeltaP2)
                                                   : static_cast<float>(stepDelta * s.deltaFactor);

            consumedDeltaP1 += substepDeltaP1;
            consumedDeltaP2 += substepDeltaP2;
            bot::rotationDelta = substepDeltaP1;

            if (bot::p1Split) {
                PlayerObject::update(substepDeltaP1);
                if (!s.endStep) {
                    if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) {
                        this->m_isOnGround = p1StartedOnGround;
                    }
                    pl->checkCollisions(this, substepDeltaP1, true);
                    PlayerObject::updateRotation(substepDeltaP1);
                    bot::resetCollisionLog(this);
                }
            } else if (s.endStep) {
                PlayerObject::update(stepDelta);
            }

            if (p2 && bot::p2Split) {
                p2->update(substepDeltaP2);
                if (!s.endStep) {
                    if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) {
                        p2->m_isOnGround = p2StartedOnGround;
                    }
                    pl->checkCollisions(p2, substepDeltaP2, true);
                    p2->updateRotation(substepDeltaP2);
                    bot::resetCollisionLog(p2);
                }
            } else if (p2 && s.endStep) {
                p2->update(stepDelta);
            }

            acc += s.deltaFactor;
            firstLoop = false;
        }

        bot::midStep = false;
        ++g.step;
    }

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

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bot::g = bot::State{};
        bot::resetEngine();
        if (level) {
            bot::g.levelID = level->m_levelID.value();
            bot::g.levelName = level->m_levelName.c_str();
            if (bot::g.levelID == 0) {
                std::hash<std::string> hasher;
                std::string basis = fmt::format("{}_{}", bot::g.levelName, level->m_levelString.size());
                bot::g.levelHash = static_cast<uint64_t>(hasher(basis));
            }
        }
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        bot::loadMacro();
        return true;
    }

    void resetLevel() {
        std::srand(1337);
        bot::g.cpLoaded = false;
        PlayLayer::resetLevel();
        bot::resetEngine();
        if (bot::g.cpLoaded) return;

        bot::g.step = 0;
        bot::g.index = 0;
        bot::g.checkpoints.clear();
        if (bot::g.mode == bot::Mode::Record) bot::g.macro.clear();
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (cp) {
            bot::CheckpointData data;
            data.step = bot::g.step;
            if (m_player1) {
                data.p1Pos = m_player1->getPosition();
                data.p1YVel = m_player1->m_yVelocity;
                data.p1Rot = m_player1->getRotation();
            }
            if (m_player2) {
                data.p2Pos = m_player2->getPosition();
                data.p2YVel = m_player2->m_yVelocity;
                data.p2Rot = m_player2->getRotation();
            }
            bot::g.checkpoints[cp] = data;
        }
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        bot::g.cpLoaded = true;

        this->m_queuedButtons.clear();
        bot::primeEngineAfterRespawn(1.0 / 60.0);

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

        auto const& data = it->second;
        bot::g.step = data.step;

        // Восстановление микро-координат и векторов скорости
        if (m_player1) {
            m_player1->setPosition(data.p1Pos);
            m_player1->m_yVelocity = data.p1YVel;
            m_player1->setRotation(data.p1Rot);
        }
        if (m_player2) {
            m_player2->setPosition(data.p2Pos);
            m_player2->m_yVelocity = data.p2YVel;
            m_player2->setRotation(data.p2Rot);
        }

        if (bot::g.mode == bot::Mode::Record) {
            auto& m = bot::g.macro;
            m.erase(
                std::remove_if(m.begin(), m.end(), [](bot::Action const& a) {
                    return a.step >= bot::g.step;
                }),
                m.end()
            );
        } else if (bot::g.mode == bot::Mode::Play) {
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
// МЕНЮ ПАУЗЫ
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
        label->setScale(0.55f);
        label->setAnchorPoint({ 0.f, 0.5f });
        label->setPosition({ 10.f, win.height / 2.f + 85.f });
        this->addChild(label);

        auto makeBtn = [this](char const* text, float scale, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(text);
            spr->setScale(scale);
            return CCMenuItemSpriteExtra::create(spr, this, cb);
        };

        float col1X = 42.f;
        float startY = win.height / 2.f + 55.f;

        // Колонка 1: Управление записью
        auto btnRec = makeBtn("REC", 0.55f, menu_selector(BotPauseLayer::onBotRec));
        btnRec->setPosition({ col1X, startY });
        menu->addChild(btnRec);

        auto btnPlay = makeBtn("PLAY", 0.55f, menu_selector(BotPauseLayer::onBotPlay));
        btnPlay->setPosition({ col1X, startY - 26.f });
        menu->addChild(btnPlay);

        auto btnStop = makeBtn("STOP", 0.55f, menu_selector(BotPauseLayer::onBotStop));
        btnStop->setPosition({ col1X, startY - 52.f });
        menu->addChild(btnStop);

        auto btnClear = makeBtn("CLEAR", 0.55f, menu_selector(BotPauseLayer::onBotClear));
        btnClear->setPosition({ col1X, startY - 78.f });
        menu->addChild(btnClear);

        // Колонка 2: Меню макросов и скорость
        float col2X = 125.f;

        auto btnManagerSpr = ButtonSprite::create("MACROS", "goldFont.fnt", "GJ_button_04.png", 0.8f);
        btnManagerSpr->setScale(0.52f);
        auto btnManager = CCMenuItemSpriteExtra::create(btnManagerSpr, this, menu_selector(BotPauseLayer::onOpenMacroManager));
        btnManager->setPosition({ col2X, startY });
        menu->addChild(btnManager);

        auto btnSpdDown = makeBtn("-0.1", 0.45f, menu_selector(BotPauseLayer::onSpeedDown));
        btnSpdDown->setPosition({ col2X - 24.f, startY - 30.f });
        menu->addChild(btnSpdDown);

        auto btnSpdUp = makeBtn("+0.1", 0.45f, menu_selector(BotPauseLayer::onSpeedUp));
        btnSpdUp->setPosition({ col2X + 24.f, startY - 30.f });
        menu->addChild(btnSpdUp);

        this->addChild(menu);
    }

    void updateBotLabel() {
        if (auto l = typeinfo_cast<CCLabelBMFont*>(this->getChildByID("bot-status"))) {
            l->setString(bot::statusText().c_str());
        }
    }

    void onOpenMacroManager(CCObject*) {
        MacroManagerPopup::create()->show();
    }

    void onSpeedDown(CCObject*) {
        bot::recSpeed = std::clamp(bot::recSpeed - 0.1, 0.05, 1.0);
        this->updateBotLabel();
    }

    void onSpeedUp(CCObject*) {
        bot::recSpeed = std::clamp(bot::recSpeed + 0.1, 0.05, 1.0);
        this->updateBotLabel();
    }

    void onBotRec(CCObject*) {
        bot::g.macro.clear();
        bot::g.mode = bot::Mode::Record;
        bot::applyClickFlags(PlayLayer::get());
        bot::resetEngine();
        bot::notify("Recording...", NotificationIcon::Info);
        this->onRestart(nullptr);
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
