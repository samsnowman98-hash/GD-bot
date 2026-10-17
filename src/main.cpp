// CBF-style TAS bot for Geometry Dash 2.2081 / Geode 5.x (Android & PC)
//
// Макрос хранится как {step, frac}: номер физического шага + точная доля шага (double 0..1).
// Честная подтиковая нарезка (Sub-tick CBF): позволяет спамить волной быстрее 240 TPS
// и поддерживает сверхкороткие клики при замедлении (0.01x).

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

    constexpr uint32_t kMagic = 0x32544F42; // "BOT2"

    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t step = 0;   // номер физического шага от начала попытки
        double frac = 0.0;   // доля шага 0..1
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

    // ---- CBF подсистема реального времени ----
    inline std::vector<PlayerButtonCommand> inputVector;
    inline double stepStartTime = 0.0;
    inline bool stepTimeInitialized = false;

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

    inline void resetEngine() {
        inputVector.clear();
        stepStartTime = getCurrentTimestamp();
        stepTimeInitialized = true;
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

    inline void drainQueuedButtons(PlayLayer* pl) {
        if (!pl) return;
        if (mode == Mode::Play) {
            pl->m_queuedButtons.clear();
            return;
        }

        double const now = getCurrentTimestamp();
        for (auto cmd : pl->m_queuedButtons) {
            if (cmd.m_timestamp > now + 5.0) cmd.m_timestamp /= 1000.0;
            inputVector.push_back(cmd);
        }
        pl->m_queuedButtons.clear();
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
            stepStartTime = getCurrentTimestamp();
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
// Контроллер времени кадра
// ---------------------------------------------------------------------------
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        bot::onFrameStart();
        CCScheduler::update(dt);
    }
};

// ---------------------------------------------------------------------------
// Перехват кнопок
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
            return;
        }

        if (applying > 0) {
            Action a;
            a.step = applyStep;
            a.frac = applyFrac;
            a.button = static_cast<uint8_t>(button);
            a.down = down;
            a.p1 = isPlayer1;
            macro.push_back(a);

            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
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
// Честная подтиковая нарезка (CBF Sub-tick Engine)
// ---------------------------------------------------------------------------
class $modify(BotPlayerObject, PlayerObject) {
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
            if (!inputThisStep || this != pl->m_player2) PlayerObject::update(stepDelta);
            return;
        }

        drainQueuedButtons(pl);

        uint32_t const cur = step;

        double const timeScale = std::max(0.001, static_cast<double>(appliedScale));
        double const stepRealDuration = std::max(1e-6, static_cast<double>(stepDelta) / timeScale);

        double const now = getCurrentTimestamp();
        if (!stepTimeInitialized || now < stepStartTime || (now - stepStartTime) > 2.0 * stepRealDuration) {
            stepStartTime = now;
            stepTimeInitialized = true;
        }

        double const stepEndTime = stepStartTime + stepRealDuration;
        std::vector<Sub> subs;

        if (mode == Mode::Play) {
            double elapsed = 0.0;
            while (playIndex < macro.size() && macro[playIndex].step <= cur) {
                auto const& a = macro[playIndex++];
                double f = (a.step < cur) ? 0.0 : std::clamp(a.frac, 0.0, 1.0);
                if (f < elapsed) f = elapsed;

                Sub s;
                s.has = true;
                s.button = a.button;
                s.push = a.down;
                s.p1 = a.p1;
                s.deltaFactor = std::max(0.0, f - elapsed);
                s.endStep = false;
                subs.push_back(s);
                elapsed = f;
            }
            if (!subs.empty()) {
                Sub e;
                e.deltaFactor = std::max(0.0, 1.0 - elapsed);
                e.endStep = true;
                subs.push_back(e);
            }
        }
        else { // Record
            std::stable_sort(inputVector.begin(), inputVector.end(), [](auto const& a, auto const& b) {
                return a.m_timestamp < b.m_timestamp;
            });

            std::vector<PlayerButtonCommand> stepCommands;
            size_t consumed = 0;
            for (size_t k = 0; k < inputVector.size(); ++k) {
                auto const& cmd = inputVector[k];
                if (cmd.m_timestamp < stepEndTime) {
                    stepCommands.push_back(cmd);
                    ++consumed;
                } else {
                    break;
                }
            }
            if (consumed > 0) {
                inputVector.erase(inputVector.begin(), inputVector.begin() + consumed);
            }

            if (!stepCommands.empty()) {
                double elapsed = 0.0;
                for (auto const& in : stepCommands) {
                    double t = std::clamp((in.m_timestamp - stepStartTime) / stepRealDuration, 0.0, 1.0);
                    if (t < elapsed) t = elapsed;

                    Sub s;
                    s.has = true;
                    s.button = static_cast<int>(in.m_button);
                    s.push = in.m_isPush;
                    s.p1 = !in.m_isPlayer2;
                    s.deltaFactor = std::max(0.0, t - elapsed);
                    s.endStep = false;
                    subs.push_back(s);

                    elapsed = t;
                }

                Sub e;
                e.deltaFactor = std::max(0.0, 1.0 - elapsed);
                e.endStep = true;
                subs.push_back(e);
            }
        }

        inputThisStep = !subs.empty();

        if (subs.empty()) {
            p1Split = false;
            p2Split = false;
            PlayerObject::update(stepDelta);
            stepStartTime += stepRealDuration;
            ++step;
            return;
        }

        PlayerObject* p2 = pl->m_player2;
        bool isDual = pl->m_gameState.m_isDualMode;
        bool p1StartedOnGround = this->m_isOnGround;
        bool p2StartedOnGround = p2 ? p2->m_isOnGround : false;

        bool p1NotBuffering = p1StartedOnGround
            || this->m_touchingRings->count()
            || this->m_isDashing
            || (this->m_isDart || this->m_isBird || this->m_isShip || this->m_isSwing);

        bool p2NotBuffering = p2 && (p2StartedOnGround
            || p2->m_touchingRings->count()
            || p2->m_isDashing
            || (p2->m_isDart || p2->m_isBird || p2->m_isShip || p2->m_isSwing));

        p1Pos = PlayerObject::getPosition();
        p2Pos = p2 ? p2->getPosition() : CCPoint{ 0.f, 0.f };

        p1Split = p1NotBuffering;
        p2Split = p2NotBuffering && isDual;

        midStep = true;
        bool firstLoop = true;
        double acc = 0.0;
        Sub pending;

        for (auto const& s : subs) {
            if (pending.has) {
                applyInput(pl, pending, cur, acc);
            }
            pending = s;

            float const substepDelta = static_cast<float>(stepDelta * s.deltaFactor);
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

            if (p2Split && p2) {
                p2->update(substepDelta);
                if (!s.endStep) {
                    if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) p2->m_isOnGround = p2StartedOnGround;
                    if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
                    else pl->checkCollisions(p2, stepDelta, true);
                    p2->updateRotation(substepDelta);
                    resetCollisionLog(p2);
                }
            }
            else if (s.endStep && p2 && isDual) p2->update(stepDelta);

            acc += s.deltaFactor;
            firstLoop = false;
        }

        if (pending.has) {
            applyInput(pl, pending, cur, std::min(1.0, acc));
        }

        midStep = false;
        stepStartTime += stepRealDuration;
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
// Уровень, чекпоинты, рестарты
// ---------------------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bot::mode = bot::Mode::Off;
        bot::step = 0;
        bot::playIndex = 0;
        bot::checkpoints.clear();
        bot::resetEngine();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        bot::applyClickFlags();

        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            Notification::create("Disable Click Between Frames mod: it conflicts with the bot", NotificationIcon::Warning, 4.f)->show();
        }
        return true;
    }

    void resetLevel() {
        bot::cpLoaded = false;
        PlayLayer::resetLevel();
        bot::resetEngine();
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
// Интерфейс меню паузы
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
