// CBF-style TAS bot for Geometry Dash 2.2081 / Geode 5.x (Android & Windows)
// Pixel-Perfect Sub-Tick Precision (<= 10 microseconds) + Anti-Stuck Key Engine

#ifdef GEODE_IS_WINDOWS
    #define NOMINMAX
    #include <windows.h>
#else
    #include <time.h>
#endif

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace bot {

    constexpr uint32_t kMagic = 0x33544F42; // Сигнатура "BOT3"

    enum class Mode { Off, Record, Play };

    struct Action {
        uint64_t id = 0;         // Монотонный ID для строгого порядка Press/Release
        uint32_t step = 0;       // Номер 240Hz физического тика
        double frac = 0.0;       // Наносекундное смещение [0.0, 1.0) внутри тика
        uint8_t button = 1;      // 1 = Jump, 2 = Left, 3 = Right
        bool down = false;       // true = Press, false = Release
        bool p1 = true;          // true = Player 1, false = Player 2

        bool operator<(Action const& o) const {
            if (step != o.step) return step < o.step;
            if (std::abs(frac - o.frac) > 1e-9) return frac < o.frac;
            return id < o.id;   // При равном времени порядок поступления кликов не нарушается
        }
    };

    struct PlayerPhysicsState {
        CCPoint pos = { 0.f, 0.f };
        CCPoint lastPos = { 0.f, 0.f };
        double yVel = 0.0;
        float rot = 0.f;
        bool upsideDown = false;
        bool onGround = false;
        bool isDashing = false;
    };

    struct CheckpointData {
        uint32_t step = 0;
        size_t macroSize = 0;
        bool held[4][2] = {};
        PlayerPhysicsState p1State;
        PlayerPhysicsState p2State;
    };

    // ---- Состояние бота ----
    inline Mode mode = Mode::Off;
    inline std::vector<Action> macro;
    inline uint64_t actionCounter = 0;
    inline uint32_t step = 0;
    inline int silent = 0;
    inline bool isSubstepping = false;
    inline bool isPlayingBack = false;

    inline double recSpeed = 1.0;
    inline float appliedScale = 1.0f;
    inline double stepStartTimestamp = 0.0;

    inline bool currentHeld[4][2] = {};
    inline std::unordered_map<CheckpointObject*, CheckpointData> checkpoints;

    // Высокоточный аппаратный таймер (наносекундный уровень)
    inline double getHighPrecisionTimestamp() {
#ifdef GEODE_IS_WINDOWS
        static LARGE_INTEGER freq;
        static bool init = false;
        if (!init) {
            QueryPerformanceFrequency(&freq);
            init = true;
        }
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
    }

    inline bool isPlayLayer(GJBaseGameLayer* l) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == l;
    }

    inline void applyClickFlags() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (mode == Mode::Record) {
            pl->m_clickBetweenSteps = true;
            pl->m_clickOnSteps = true;
        } else if (mode == Mode::Play) {
            pl->m_clickBetweenSteps = false;
            pl->m_clickOnSteps = false;
        } else {
            auto gm = GameManager::get();
            pl->m_clickBetweenSteps = gm->getGameVariable("0177");
            pl->m_clickOnSteps = gm->getGameVariable("0176");
        }
    }

    inline void setTimeScale(float s) {
        if (std::fabs(s - appliedScale) < 1e-6f) return;
        appliedScale = s;
        CCDirector::sharedDirector()->getScheduler()->setTimeScale(s);
    }

    inline double parseSpeedString(std::string const& str) {
        std::string s = str;
        for (char& c : s) if (c == ',') c = '.';
        try {
            double val = std::stod(s);
            return std::clamp(val, 0.0001, 2.0);
        } catch (...) {
            return 1.0;
        }
    }

    inline std::filesystem::path getMacroDir() {
        auto dir = Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // Кроссплатформенный безопасный сброс зажатий (Android NDK + Windows)
    inline void forceReleaseEverything(PlayLayer* pl) {
        if (!pl) return;
        ++silent;
        for (int b = 1; b <= 3; ++b) {
            pl->handleButton(false, b, true);
            pl->handleButton(false, b, false);
            currentHeld[b][0] = false;
            currentHeld[b][1] = false;
        }
        pl->m_queuedButtons.clear();
        --silent;
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
            out.write(reinterpret_cast<char const*>(&a.id), 8);
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
        uint64_t maxId = 0;
        for (uint32_t i = 0; i < count; ++i) {
            Action a;
            uint8_t flags = 0;
            in.read(reinterpret_cast<char*>(&a.id), 8);
            in.read(reinterpret_cast<char*>(&a.step), 4);
            in.read(reinterpret_cast<char*>(&a.frac), 8);
            in.read(reinterpret_cast<char*>(&flags), 1);
            in.read(reinterpret_cast<char*>(&a.button), 1);
            if (!in) return false;
            a.down = (flags & 1) != 0;
            a.p1 = (flags & 2) != 0;
            if (a.id > maxId) maxId = a.id;
            tmp.push_back(a);
        }
        actions = std::move(tmp);
        actionCounter = maxId + 1;
        std::sort(actions.begin(), actions.end());
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
            } else {
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

    inline void setMode(Mode m) {
        mode = m;
        applyClickFlags();
        if (mode == Mode::Off) {
            setTimeScale(1.0f);
        }
        forceReleaseEverything(PlayLayer::get());
    }
}

// ---------------------------------------------------------------------------
// Перехват ввода (Вычисление CBF смещения с наносекундной точностью)
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
            if (isPlayingBack) {
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
            }
            return; // Внешний физический ввод блокируется при воспроизведении
        }

        if (mode == Mode::Record) {
            auto pl = PlayLayer::get();
            if (pl && (pl->m_playerDied || pl->m_isPaused)) return;

            double now = getHighPrecisionTimestamp();
            float timewarp = (pl ? pl->m_gameState.m_timeWarp : 1.0f);
            if (timewarp <= 0.0001f) timewarp = 1.0f;

            double stepDuration = (1.0 / 240.0) * timewarp / std::max(0.0001, recSpeed);
            double elapsed = now - stepStartTimestamp;
            double frac = 0.0;
            if (stepDuration > 0.0) {
                frac = std::clamp(elapsed / stepDuration, 0.0, 0.999999999);
            }

            Action a;
            a.id = ++actionCounter;
            a.step = step;
            a.frac = frac; // Абсолютная суб-тиковая точность без TPS-округления
            a.button = static_cast<uint8_t>(button);
            a.down = down;
            a.p1 = isPlayer1;

            macro.push_back(a);
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (bot::mode == bot::Mode::Play && bot::isPlayLayer(this)) {
            this->m_queuedButtons.clear();
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------------------
// Суб-тиковая нарезка физики
// ---------------------------------------------------------------------------
class $modify(BotPlayerObject, PlayerObject) {
    void update(float dt) {
        using namespace bot;
        auto pl = PlayLayer::get();

        if (mode == bot::Mode::Off || !pl || dt <= 0.0f || isSubstepping) {
            PlayerObject::update(dt);
            return;
        }

        bool const isP1 = (this == pl->m_player1);
        bool const isP2 = (this == pl->m_player2);
        if (!isP1 && !isP2) {
            PlayerObject::update(dt);
            return;
        }

        // P1 всегда начинает шаг тика, P2 идет следом в том же шаге
        if (isP1) {
            ++step;
            stepStartTimestamp = getHighPrecisionTimestamp();
        }

        uint32_t const curStep = step;

        if (mode == Mode::Record) {
            PlayerObject::update(dt);
            return;
        }

        // ---- Режим PLAY: суб-тиковое деление шага ----
        auto it = std::lower_bound(
            macro.begin(), macro.end(), curStep,
            [](Action const& a, uint32_t s) { return a.step < s; }
        );

        std::vector<Action> stepActions;
        while (it != macro.end() && it->step == curStep) {
            if (it->p1 == isP1) {
                stepActions.push_back(*it);
            }
            ++it;
        }

        if (stepActions.empty()) {
            PlayerObject::update(dt);
            return;
        }

        std::sort(stepActions.begin(), stepActions.end());

        isSubstepping = true;
        isPlayingBack = true;

        double const totalStepDuration = static_cast<double>(dt);
        double accumulatedFrac = 0.0;
        double spentDtDouble = 0.0;

        for (auto const& act : stepActions) {
            double currentFrac = std::clamp(act.frac, 0.0, 1.0);
            double deltaFrac = currentFrac - accumulatedFrac;

            if (deltaFrac > 1e-9) {
                double subStepDouble = totalStepDuration * deltaFrac;
                spentDtDouble += subStepDouble;
                PlayerObject::update(static_cast<float>(subStepDouble));
            }

            pl->handleButton(act.down, act.button, act.p1);
            accumulatedFrac = currentFrac;
        }

        // Доигрывание остатка времени шага (компенсация float)
        double remainingDouble = totalStepDuration - spentDtDouble;
        if (remainingDouble > 1e-9) {
            PlayerObject::update(static_cast<float>(remainingDouble));
        }

        isPlayingBack = false;
        isSubstepping = false;
    }
};

// ---------------------------------------------------------------------------
// Контроль уровня и чекпоинтов
// ---------------------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    void visit() {
        if (bot::mode == bot::Mode::Record && bot::recSpeed < 0.95 && this->m_player1 && !this->m_playerDied && !this->m_isPaused) {
            double now = bot::getHighPrecisionTimestamp();
            float timewarp = this->m_gameState.m_timeWarp <= 0.0001f ? 1.0f : this->m_gameState.m_timeWarp;
            double stepDuration = (1.0 / 240.0) * timewarp / std::max(0.0001, bot::recSpeed);
            double elapsed = now - bot::stepStartTimestamp;
            float alpha = std::clamp(static_cast<float>(elapsed / stepDuration), 0.0f, 1.0f);

            auto p1 = this->m_player1;
            CCPoint orig1 = p1->getPosition();
            CCPoint d1 = orig1 - p1->m_lastPosition;
            p1->setPosition(orig1 + d1 * alpha);

            CCPoint orig2 = { 0.f, 0.f };
            auto p2 = this->m_player2;
            if (p2) {
                orig2 = p2->getPosition();
                CCPoint d2 = orig2 - p2->m_lastPosition;
                p2->setPosition(orig2 + d2 * alpha);
            }

            PlayLayer::visit();

            p1->setPosition(orig1);
            if (p2) p2->setPosition(orig2);
            return;
        }

        PlayLayer::visit();
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bot::setMode(bot::Mode::Off);
        bot::step = 0;
        bot::actionCounter = 0;
        bot::checkpoints.clear();
        bot::macro.clear();
        srand(1337);

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            Notification::create("Disable Click Between Frames mod to prevent TAS conflict", NotificationIcon::Warning, 4.f)->show();
        }
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        srand(1337);
        bot::forceReleaseEverything(this);

        if (!this->m_checkpointArray || this->m_checkpointArray->count() == 0) {
            bot::step = 0;
            bot::checkpoints.clear();
            if (bot::mode == bot::Mode::Record) bot::macro.clear();
        }
        bot::stepStartTimestamp = bot::getHighPrecisionTimestamp();
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (cp && this->m_player1) {
            bot::CheckpointData data;
            data.step = bot::step;
            data.macroSize = bot::macro.size();

            for (int b = 1; b <= 3; ++b) {
                data.held[b][0] = bot::currentHeld[b][0];
                data.held[b][1] = bot::currentHeld[b][1];
            }

            auto saveState = [](PlayerObject* p, bot::PlayerPhysicsState& s) {
                if (!p) return;
                s.pos = p->getPosition();
                s.lastPos = p->m_lastPosition;
                s.yVel = p->m_yVelocity;
                s.rot = p->getRotation();
                s.upsideDown = p->m_isUpsideDown;
                s.onGround = p->m_isOnGround;
                s.isDashing = p->m_isDashing;
            };

            saveState(this->m_player1, data.p1State);
            if (this->m_player2) {
                saveState(this->m_player2, data.p2State);
            }
            bot::checkpoints[cp] = data;
        }
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        auto it = bot::checkpoints.find(cp);
        if (it == bot::checkpoints.end()) {
            if (bot::mode != bot::Mode::Off) {
                bot::setMode(bot::Mode::Off);
                Notification::create("Unknown checkpoint! Bot OFF", NotificationIcon::Warning)->show();
            }
            return;
        }

        auto const& data = it->second;
        bot::step = data.step;

        // Синхронизация вектора макроса
        if (bot::mode == bot::Mode::Record) {
            if (data.macroSize <= bot::macro.size()) {
                bot::macro.erase(bot::macro.begin() + data.macroSize, bot::macro.end());
            } else {
                uint32_t s = data.step;
                bot::macro.erase(
                    std::remove_if(bot::macro.begin(), bot::macro.end(), [s](bot::Action const& a) { return a.step >= s; }),
                    bot::macro.end()
                );
            }
        }

        // Восстановление физического вектора
        auto restoreState = [](PlayerObject* p, bot::PlayerPhysicsState const& s) {
            if (!p) return;
            p->setPosition(s.pos);
            p->m_lastPosition = s.lastPos;
            p->m_yVelocity = s.yVel;
            p->setRotation(s.rot);
            p->m_isUpsideDown = s.upsideDown;
            p->m_isOnGround = s.onGround;
            p->m_isDashing = s.isDashing;
        };

        restoreState(this->m_player1, data.p1State);
        if (this->m_player2) {
            restoreState(this->m_player2, data.p2State);
        }

        // АНТИ-ЗАЛИПАНИЕ: В режиме записи отпускаем всё, чтобы палец человека не десинкался с игрой!
        if (bot::mode == bot::Mode::Record) {
            bot::forceReleaseEverything(this);
        } else if (bot::mode == bot::Mode::Play) {
            ++bot::silent;
            for (int b = 1; b <= 3; ++b) {
                bot::currentHeld[b][0] = data.held[b][0];
                bot::currentHeld[b][1] = data.held[b][1];
                this->handleButton(data.held[b][0], b, true);
                this->handleButton(data.held[b][1], b, false);
            }
            --bot::silent;
        }

        bot::stepStartTimestamp = bot::getHighPrecisionTimestamp();
        this->m_queuedButtons.clear();
        srand(1337 + data.step);
    }
};

// ---------------------------------------------------------------------------
// GUI в PauseLayer
// ---------------------------------------------------------------------------
class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCNode* m_fileContainer = nullptr;
        CCLabelBMFont* m_statusLabel = nullptr;
        ButtonSprite* m_offSpr = nullptr;
        ButtonSprite* m_recSpr = nullptr;
        ButtonSprite* m_playSpr = nullptr;
        TextInput* m_speedInput = nullptr;
    };

    void updateStatusUI() {
        if (m_fields->m_statusLabel) {
            auto stats = bot::macroStats();
            std::string minStr = stats.minHold >= 0.0 ? fmt::format("{:.4f}", stats.minHold) : "-";
            std::string info = fmt::format("{} acts | P:{}/R:{} | min: {}", bot::macro.size(), stats.pushes, stats.releases, minStr);
            std::string title = "OFF";
            if (bot::mode == bot::Mode::Record) title = "REC " + info;
            else if (bot::mode == bot::Mode::Play) title = "PLAY " + info;
            else if (!bot::macro.empty()) title = "READY " + info;
            m_fields->m_statusLabel->setString(title.c_str());
        }
        if (m_fields->m_offSpr) {
            m_fields->m_offSpr->setColor(bot::mode == bot::Mode::Off ? ccColor3B{ 80, 255, 80 } : ccColor3B{ 255, 255, 255 });
        }
        if (m_fields->m_recSpr) {
            m_fields->m_recSpr->setColor(bot::mode == bot::Mode::Record ? ccColor3B{ 255, 80, 80 } : ccColor3B{ 255, 255, 255 });
        }
        if (m_fields->m_playSpr) {
            m_fields->m_playSpr->setColor(bot::mode == bot::Mode::Play ? ccColor3B{ 80, 200, 255 } : ccColor3B{ 255, 255, 255 });
        }
    }

    void refreshList() {
        if (!m_fields->m_fileContainer) return;
        m_fields->m_fileContainer->removeAllChildren();

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

        float const viewW = 160.0f;
        float const viewH = 100.0f;
        float const rowH = 22.0f;
        float const totalH = std::max(viewH, static_cast<float>(files.size()) * rowH);

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
            lbl->setAnchorPoint({ 0.0f, 0.5f });
            lbl->setPosition(4.0f, rowH / 2.0f);
            rowNode->addChild(lbl);

            auto rowMenu = CCMenu::create();
            rowMenu->setPosition(0.0f, 0.0f);

            auto loadSpr = ButtonSprite::create("L", 12, true, "goldFont.fnt", "GJ_button_01.png", 16.0f, 0.4f);
            auto loadBtn = CCMenuItemExt::createSpriteExtra(loadSpr, [this, path](CCObject*) {
                if (bot::loadMacro(path, bot::macro)) {
                    bot::setMode(bot::Mode::Off);
                    this->updateStatusUI();
                    Notification::create("Loaded successfully", NotificationIcon::Success)->show();
                } else {
                    Notification::create("Failed to load", NotificationIcon::Error)->show();
                }
            });
            loadBtn->setPosition(120.0f, rowH / 2.0f);
            rowMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("X", 12, true, "goldFont.fnt", "GJ_button_06.png", 16.0f, 0.4f);
            auto delBtn = CCMenuItemExt::createSpriteExtra(delSpr, [this, path](CCObject*) {
                std::error_code ec2;
                std::filesystem::remove(path, ec2);
                this->refreshList();
                Notification::create("Deleted", NotificationIcon::Info)->show();
            });
            delBtn->setPosition(145.0f, rowH / 2.0f);
            rowMenu->addChild(delBtn);

            rowNode->addChild(rowMenu);
            scroll->m_contentLayer->addChild(rowNode);
        }

        scroll->scrollToTop();
        m_fields->m_fileContainer->addChild(scroll);
    }

    void customSetup() {
        PauseLayer::customSetup();
        if (!PlayLayer::get()) return;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        float panelW = 170.0f;
        float panelH = 260.0f;
        float panelX = winSize.width - panelW - 8.0f;
        float panelY = (winSize.height - panelH) / 2.0f;

        auto panel = CCLayerColor::create(ccc4(0, 0, 0, 190), panelW, panelH);
        panel->setPosition(panelX, panelY);
        this->addChild(panel, 100);

        auto title = CCLabelBMFont::create("CBF TAS BOT", "goldFont.fnt");
        title->setScale(0.5f);
        title->setPosition(panelW / 2.0f, panelH - 16.0f);
        panel->addChild(title);

        m_fields->m_statusLabel = CCLabelBMFont::create("OFF", "chatFont.fnt");
        m_fields->m_statusLabel->setScale(0.40f);
        m_fields->m_statusLabel->setPosition(panelW / 2.0f, panelH - 34.0f);
        panel->addChild(m_fields->m_statusLabel);

        auto modeMenu = CCMenu::create();
        modeMenu->setPosition(0.0f, 0.0f);

        m_fields->m_offSpr = ButtonSprite::create("OFF", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto offBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_offSpr, [this](CCObject*) {
            bot::setMode(bot::Mode::Off);
            this->updateStatusUI();
        });
        offBtn->setPosition(panelW * 0.2f, panelH - 58.0f);
        modeMenu->addChild(offBtn);

        m_fields->m_recSpr = ButtonSprite::create("REC", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto recBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_recSpr, [this](CCObject*) {
            bot::macro.clear();
            bot::actionCounter = 0;
            bot::setMode(bot::Mode::Record);
            bot::setTimeScale(static_cast<float>(bot::recSpeed));
            this->updateStatusUI();
            this->onRestart(nullptr);
        });
        recBtn->setPosition(panelW * 0.5f, panelH - 58.0f);
        modeMenu->addChild(recBtn);

        m_fields->m_playSpr = ButtonSprite::create("PLAY", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto playBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_playSpr, [this](CCObject*) {
            if (bot::macro.empty()) {
                Notification::create("Macro empty!", NotificationIcon::Warning)->show();
                return;
            }
            bot::recSpeed = 1.0;
            bot::setTimeScale(1.0f);
            if (m_fields->m_speedInput) m_fields->m_speedInput->setString("1.00");
            bot::setMode(bot::Mode::Play);
            this->updateStatusUI();
            this->onRestart(nullptr);
        });
        playBtn->setPosition(panelW * 0.8f, panelH - 58.0f);
        modeMenu->addChild(playBtn);
        panel->addChild(modeMenu);

        auto speedLbl = CCLabelBMFont::create("Speed:", "chatFont.fnt");
        speedLbl->setScale(0.45f);
        speedLbl->setAnchorPoint({ 0.0f, 0.5f });
        speedLbl->setPosition(10.0f, panelH - 84.0f);
        panel->addChild(speedLbl);

        auto speedInput = TextInput::create(65.0f, "1.00", "chatFont.fnt");
        speedInput->setFilter("0123456789.,");
        speedInput->setScale(0.7f);
        speedInput->setPosition(100.0f, panelH - 84.0f);
        speedInput->setCallback([](std::string const& str) {
            bot::recSpeed = bot::parseSpeedString(str);
            if (bot::mode == bot::Mode::Record) {
                bot::setTimeScale(static_cast<float>(bot::recSpeed));
            }
        });
        speedInput->setString(fmt::format("{:.2f}", bot::recSpeed));
        m_fields->m_speedInput = speedInput;
        panel->addChild(speedInput);

        auto saveMenu = CCMenu::create();
        saveMenu->setPosition(0.0f, 0.0f);
        auto saveSpr = ButtonSprite::create("SAVE MACRO", 80, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](CCObject*) {
            if (bot::macro.empty()) {
                Notification::create("Empty macro", NotificationIcon::Warning)->show();
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
                this->refreshList();
                Notification::create("Saved successfully!", NotificationIcon::Success)->show();
            }
        });
        saveBtn->setPosition(panelW / 2.0f, panelH - 110.0f);
        saveMenu->addChild(saveBtn);
        panel->addChild(saveMenu);

        m_fields->m_fileContainer = CCNode::create();
        m_fields->m_fileContainer->setPosition(5.0f, 15.0f);
        m_fields->m_fileContainer->setContentSize({ 160.0f, 100.0f });
        panel->addChild(m_fields->m_fileContainer);

        this->updateStatusUI();
        this->refreshList();
    }
};
