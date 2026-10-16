#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Layout.hpp>
#include <Geode/loader/Mod.hpp>

#include <chrono>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <ctime>
#include <filesystem>

using namespace geode::prelude;

namespace bot {

constexpr double SMALLEST = 0.000001;
constexpr const char* kMacroExt = ".gdbot";
constexpr uint32_t kMagic = 0x32544F42;

enum class Mode { Off, Record, Play };

struct Action {
    uint32_t step;
    double frac;
    uint8_t button;
    bool down;
    bool p1;
};

struct CheckpointData {
    uint32_t step = 0;
    CCPoint p1Pos;
    double p1YVel = 0.0;
    float p1Rot = 0.0f;
    bool p1OnGround = true;
    CCPoint p2Pos;
    double p2YVel = 0.0;
    float p2Rot = 0.0f;
    bool p2OnGround = true;
};

struct State {
    Mode mode = Mode::Off;
    uint32_t step = 0;
    size_t index = 0;
    int applying = 0;
    bool cpLoaded = false;
    int levelID = 0;
    std::string levelName;
    uint64_t levelHash = 0;
    std::vector<Action> macro;
    std::unordered_map<CheckpointObject*, CheckpointData> checkpoints;
};

inline State g;

inline double stepStartTime = 0.0;
inline double recSpeed = 1.0;

inline double getCurrentTimestamp() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

inline void resetTimingBaseline() {
    stepStartTime = getCurrentTimestamp();
}

inline void resetEngine() {
    stepStartTime = getCurrentTimestamp();
}

inline const char* modeName() {
    switch (g.mode) {
        case Mode::Record: return "REC";
        case Mode::Play: return "PLAY";
        default: return "OFF";
    }
}

inline std::string statusText() {
    return fmt::format("Bot: {} | acts: {} | Spd: {:.2f}x", modeName(), g.macro.size(), recSpeed);
}

inline void notify(const std::string& text, NotificationIcon icon) {
    Notification::create(text, icon)->show();
}

inline void applyClickFlags(PlayLayer* pl) {
    if (!pl) return;
    if (g.mode != Mode::Off) {
        pl->m_clickBetweenSteps = true;
        pl->m_clickOnSteps = false;
    } else {
        auto gm = GameManager::get();
        pl->m_clickBetweenSteps = gm->getGameVariable("0177");
        pl->m_clickOnSteps = gm->getGameVariable("0176");
    }
}

inline std::filesystem::path macroPath(const std::string& filename) {
    return Mod::get()->getSaveDir() / filename;
}

inline bool writeMacroToFile(const std::filesystem::path& path, const std::vector<Action>& macro) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    uint32_t magic = kMagic;
    f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    uint32_t count = static_cast<uint32_t>(macro.size());
    f.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (auto& a : macro) {
        f.write(reinterpret_cast<const char*>(&a.step), sizeof(a.step));
        f.write(reinterpret_cast<const char*>(&a.frac), sizeof(a.frac));
        uint8_t flags = 0;
        if (a.down) flags |= 0x1;
        if (a.p1) flags |= 0x2;
        f.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
        f.write(reinterpret_cast<const char*>(&a.button), sizeof(a.button));
    }
    return true;
}

inline bool readMacroFromFile(const std::filesystem::path& path, std::vector<Action>& macro) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    uint32_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != kMagic) return false;
    uint32_t count = 0;
    f.read(reinterpret_cast<char*>(&count), sizeof(count));
    macro.clear();
    macro.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        Action a{};
        f.read(reinterpret_cast<char*>(&a.step), sizeof(a.step));
        f.read(reinterpret_cast<char*>(&a.frac), sizeof(a.frac));
        uint8_t flags = 0;
        f.read(reinterpret_cast<char*>(&flags), sizeof(flags));
        a.down = (flags & 0x1) != 0;
        a.p1 = (flags & 0x2) != 0;
        f.read(reinterpret_cast<char*>(&a.button), sizeof(a.button));
        macro.push_back(a);
    }
    return true;
}

inline std::string autoFilename() {
    if (g.levelID != 0) {
        return fmt::format("level_{}{}", g.levelID, kMacroExt);
    }
    return fmt::format("local_{}{}", g.levelHash, kMacroExt);
}

inline bool saveMacro() {
    if (g.macro.empty()) return false;
    return writeMacroToFile(macroPath(autoFilename()), g.macro);
}

inline bool loadMacro() {
    std::vector<Action> macro;
    if (!readMacroFromFile(macroPath(autoFilename()), macro)) return false;
    g.macro = std::move(macro);
    return true;
}

inline void syncPlayback(PlayLayer* pl) {
    if (!pl) return;
    auto it = std::lower_bound(g.macro.begin(), g.macro.end(), g.step,
        [](const Action& a, uint32_t step) { return a.step < step; });
    g.index = static_cast<size_t>(it - g.macro.begin());

    bool held[4][2] = { { false, false }, { false, false }, { false, false }, { false, false } };
    for (size_t i = 0; i < g.index; i++) {
        auto& a = g.macro[i];
        if (a.button >= 1 && a.button <= 4) {
            held[a.button - 1][a.p1 ? 0 : 1] = a.down;
        }
    }
    for (int b = 0; b < 4; b++) {
        for (int pIdx = 0; pIdx < 2; pIdx++) {
            if (held[b][pIdx]) {
                g.applying++;
                pl->handleButton(true, b + 1, pIdx == 0);
                g.applying--;
            }
        }
    }
}

inline void setTimeScale(double s) {
    CCDirector::get()->getScheduler()->setTimeScale(static_cast<float>(s));
}

inline void onFrameStart() {
    if (g.mode != Mode::Record) {
        setTimeScale(1.0);
        return;
    }
    auto pl = PlayLayer::get();
    if (!pl || pl->m_isPaused || pl->m_levelEndAnimationStarted) {
        setTimeScale(1.0);
        return;
    }
    setTimeScale(recSpeed);
}

}

class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        bot::onFrameStart();
        CCScheduler::update(dt);
    }
};

class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (bot::g.mode == bot::Mode::Off || static_cast<GJBaseGameLayer*>(pl) != this) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (bot::g.mode == bot::Mode::Play) {
            if (bot::g.applying > 0) {
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
            }
            return;
        }

        if (bot::g.mode == bot::Mode::Record) {
            if (bot::g.applying == 0) {
                double now = bot::getCurrentTimestamp();
                double stepDuration = (1.0 / 240.0) / (bot::recSpeed > bot::SMALLEST ? bot::recSpeed : 1.0);
                double frac = std::clamp((now - bot::stepStartTime) / stepDuration, 0.0, 0.999);

                bot::Action a{};
                a.step = bot::g.step;
                a.frac = frac;
                a.button = static_cast<uint8_t>(button);
                a.down = down;
                a.p1 = isPlayer1;
                bot::g.macro.push_back(a);
            }
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(BotPlayerObject, PlayerObject) {
    void update(float stepDelta) {
        auto pl = PlayLayer::get();
        if (!pl || bot::g.mode == bot::Mode::Off) {
            PlayerObject::update(stepDelta);
            return;
        }

        bool isP1 = (this == pl->m_player1);
        bool isP2 = (this == pl->m_player2);
        if (!isP1 && !isP2) {
            PlayerObject::update(stepDelta);
            return;
        }

        if (bot::g.mode == bot::Mode::Play) {
            std::vector<bot::Action> myActions;
            for (size_t i = bot::g.index; i < bot::g.macro.size() && bot::g.macro[i].step == bot::g.step; ++i) {
                if (bot::g.macro[i].p1 == isP1) {
                    myActions.push_back(bot::g.macro[i]);
                }
            }

            std::sort(myActions.begin(), myActions.end(), [](const bot::Action& a, const bot::Action& b) {
                return a.frac < b.frac;
            });

            if (myActions.empty()) {
                PlayerObject::update(stepDelta);
            } else {
                double currentFrac = 0.0;
                for (auto& a : myActions) {
                    double targetFrac = std::clamp(a.frac, 0.0, 1.0);
                    double subDelta = (targetFrac - currentFrac) * stepDelta;
                    if (subDelta > bot::SMALLEST) {
                        PlayerObject::update(static_cast<float>(subDelta));
                        pl->checkCollisions(this, 0.0f, isP1);
                        currentFrac = targetFrac;
                    }
                    bot::g.applying++;
                    pl->handleButton(a.down, a.button, a.p1);
                    bot::g.applying--;
                }
                double rem = (1.0 - currentFrac) * stepDelta;
                if (rem > 0.0) {
                    PlayerObject::update(static_cast<float>(rem));
                }
            }
        } else {
            PlayerObject::update(stepDelta);
        }

        bool isDual = (pl->m_player2 != nullptr && pl->m_player2->isVisible());
        if (!isDual && isP1) {
            if (bot::g.mode == bot::Mode::Play) {
                while (bot::g.index < bot::g.macro.size() && bot::g.macro[bot::g.index].step <= bot::g.step) {
                    bot::g.index++;
                }
            }
            bot::g.step++;
            bot::stepStartTime = bot::getCurrentTimestamp();
        } else if (isDual && isP2) {
            if (bot::g.mode == bot::Mode::Play) {
                while (bot::g.index < bot::g.macro.size() && bot::g.macro[bot::g.index].step <= bot::g.step) {
                    bot::g.index++;
                }
            }
            bot::g.step++;
            bot::stepStartTime = bot::getCurrentTimestamp();
        }
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bot::g = bot::State{};
        bot::resetEngine();

        bool ret = PlayLayer::init(level, useReplay, dontCreateObjects);
        if (!ret) return false;

        bot::g.levelID = level ? static_cast<int>(level->m_levelID) : 0;
        bot::g.levelName = level ? std::string(level->m_levelName.c_str()) : std::string("unknown");
        std::string hashSource = fmt::format("{}_{}", bot::g.levelName, level ? level->m_levelString.size() : 0);
        bot::g.levelHash = std::hash<std::string>{}(hashSource);

        bot::loadMacro();
        bot::applyClickFlags(this);

        return true;
    }

    void resetLevel() {
        std::srand(1337);
        bot::g.cpLoaded = false;

        PlayLayer::resetLevel();

        bot::resetEngine();

        if (!bot::g.cpLoaded) {
            bot::g.step = 0;
            bot::g.index = 0;
            bot::g.checkpoints.clear();
            if (bot::g.mode == bot::Mode::Record) {
                bot::g.macro.clear();
            }
        }
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (cp) {
            bot::CheckpointData data{};
            data.step = bot::g.step;
            if (this->m_player1) {
                data.p1Pos = this->m_player1->getPosition();
                data.p1YVel = this->m_player1->m_yVelocity;
                data.p1Rot = this->m_player1->getRotation();
                data.p1OnGround = this->m_player1->m_isOnGround;
            }
            if (this->m_player2) {
                data.p2Pos = this->m_player2->getPosition();
                data.p2YVel = this->m_player2->m_yVelocity;
                data.p2Rot = this->m_player2->getRotation();
                data.p2OnGround = this->m_player2->m_isOnGround;
            }
            bot::g.checkpoints[cp] = data;
        }
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);

        bot::g.cpLoaded = true;
        this->m_playerDied = false;

        this->handleButton(false, 1, true);
        this->handleButton(false, 1, false);

        bot::resetTimingBaseline();

        auto it = bot::g.checkpoints.find(cp);
        if (it == bot::g.checkpoints.end()) {
            bot::g.step = 0;
            bot::g.index = 0;
            if (bot::g.mode != bot::Mode::Off) {
                bot::g.mode = bot::Mode::Off;
                bot::applyClickFlags(this);
                bot::notify("Bot: checkpoint data not found, disabled", NotificationIcon::Warning);
            }
            return;
        }

        auto& data = it->second;
        bot::g.step = data.step;

        if (this->m_player1) {
            this->m_player1->setPosition(data.p1Pos);
            this->m_player1->m_yVelocity = data.p1YVel;
            this->m_player1->setRotation(data.p1Rot);
            this->m_player1->m_isOnGround = data.p1OnGround;
            this->m_player1->m_isDead = false;
        }
        if (this->m_player2) {
            this->m_player2->setPosition(data.p2Pos);
            this->m_player2->m_yVelocity = data.p2YVel;
            this->m_player2->setRotation(data.p2Rot);
            this->m_player2->m_isOnGround = data.p2OnGround;
            this->m_player2->m_isDead = false;
        }

        if (bot::g.mode == bot::Mode::Record) {
            bot::g.macro.erase(
                std::remove_if(bot::g.macro.begin(), bot::g.macro.end(),
                    [](const bot::Action& a) { return a.step >= bot::g.step; }),
                bot::g.macro.end()
            );
        } else if (bot::g.mode == bot::Mode::Play) {
            bot::syncPlayback(this);
        }
    }

    void levelComplete() {
        if (bot::g.mode == bot::Mode::Record && !bot::g.macro.empty()) {
            bot::saveMacro();
            bot::notify("Macro saved", NotificationIcon::Success);
        }

        bot::g.mode = bot::Mode::Off;
        bot::applyClickFlags(this);
        bot::resetEngine();

        PlayLayer::levelComplete();
    }
};

class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCMenu* m_mainBotMenu = nullptr;
        CCNode* m_macroPanel = nullptr;
        ScrollLayer* m_scroll = nullptr;
        CCMenu* m_listMenu = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::get()->getWinSize();

        auto mainMenu = CCMenu::create();
        mainMenu->setPosition({ winSize.width / 2.0f, 42.0f });
        mainMenu->setContentSize({ winSize.width - 40.0f, 32.0f });

        auto layout = RowLayout::create();
        layout->setGap(6.0f);
        layout->setAxisAlignment(AxisAlignment::Center);
        mainMenu->setLayout(layout);
        this->addChild(mainMenu, 100);
        m_fields->m_mainBotMenu = mainMenu;

        auto recSpr = ButtonSprite::create("REC");
        recSpr->setScale(0.55f);
        auto recBtn = CCMenuItemSpriteExtra::create(recSpr, this, menu_selector(BotPauseLayer::onBotRec));
        mainMenu->addChild(recBtn);

        auto playSpr = ButtonSprite::create("PLAY");
        playSpr->setScale(0.55f);
        auto playBtn = CCMenuItemSpriteExtra::create(playSpr, this, menu_selector(BotPauseLayer::onBotPlay));
        mainMenu->addChild(playBtn);

        auto stopSpr = ButtonSprite::create("STOP");
        stopSpr->setScale(0.55f);
        auto stopBtn = CCMenuItemSpriteExtra::create(stopSpr, this, menu_selector(BotPauseLayer::onBotStop));
        mainMenu->addChild(stopBtn);

        auto clearSpr = ButtonSprite::create("CLEAR");
        clearSpr->setScale(0.55f);
        auto clearBtn = CCMenuItemSpriteExtra::create(clearSpr, this, menu_selector(BotPauseLayer::onBotClear));
        mainMenu->addChild(clearBtn);

        auto macrosSpr = ButtonSprite::create("MACROS");
        macrosSpr->setScale(0.55f);
        auto macrosBtn = CCMenuItemSpriteExtra::create(macrosSpr, this, menu_selector(BotPauseLayer::onOpenMacroPanel));
        mainMenu->addChild(macrosBtn);

        auto speedDownSpr = ButtonSprite::create("-");
        speedDownSpr->setScale(0.55f);
        auto speedDownBtn = CCMenuItemSpriteExtra::create(speedDownSpr, this, menu_selector(BotPauseLayer::onSpeedDown));
        mainMenu->addChild(speedDownBtn);

        auto speedUpSpr = ButtonSprite::create("+");
        speedUpSpr->setScale(0.55f);
        auto speedUpBtn = CCMenuItemSpriteExtra::create(speedUpSpr, this, menu_selector(BotPauseLayer::onSpeedUp));
        mainMenu->addChild(speedUpBtn);

        mainMenu->updateLayout();

        auto label = CCLabelBMFont::create(bot::statusText().c_str(), "bigFont.fnt");
        label->setScale(0.35f);
        label->setPosition({ winSize.width / 2.0f, 18.0f });
        label->setID("bot-status");
        this->addChild(label, 100);

        auto panel = CCNode::create();
        panel->setContentSize({ 320, 220 });
        panel->setAnchorPoint({ 0.5f, 0.5f });
        panel->setPosition({ winSize.width / 2.0f, winSize.height / 2.0f });
        panel->setVisible(false);
        this->addChild(panel, 200);
        m_fields->m_macroPanel = panel;

        auto bg = CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({ 320, 220 });
        bg->setPosition({ 160, 110 });
        bg->setOpacity(230);
        panel->addChild(bg);

        auto title = CCLabelBMFont::create("Saved Macros", "bigFont.fnt");
        title->setScale(0.55f);
        title->setPosition({ 160, 200 });
        panel->addChild(title);

        auto scroll = ScrollLayer::create({ 300, 140 });
        scroll->setPosition({ 10, 45 });
        panel->addChild(scroll);
        m_fields->m_scroll = scroll;

        auto panelMenu = CCMenu::create();
        panelMenu->setPosition({ 0, 0 });
        panel->addChild(panelMenu);

        auto saveSpr = ButtonSprite::create("SAVE CURRENT");
        saveSpr->setScale(0.6f);
        auto saveBtn = CCMenuItemSpriteExtra::create(saveSpr, this, menu_selector(BotPauseLayer::onSaveCurrentMacro));
        saveBtn->setPosition({ 90, 22 });
        panelMenu->addChild(saveBtn);

        auto backSpr = ButtonSprite::create("BACK");
        backSpr->setScale(0.6f);
        auto backBtn = CCMenuItemSpriteExtra::create(backSpr, this, menu_selector(BotPauseLayer::onCloseMacroPanel));
        backBtn->setPosition({ 230, 22 });
        panelMenu->addChild(backBtn);

        this->refreshMacroList();
    }

    void refreshMacroList() {
        auto scroll = m_fields->m_scroll;
        if (!scroll) return;

        auto content = scroll->m_contentLayer;
        content->removeAllChildren();

        auto listMenu = CCMenu::create();
        listMenu->setPosition({ 0, 0 });
        content->addChild(listMenu);
        m_fields->m_listMenu = listMenu;

        std::vector<std::filesystem::path> files;
        auto dir = Mod::get()->getSaveDir();
        if (std::filesystem::exists(dir)) {
            for (auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == bot::kMacroExt) {
                    files.push_back(entry.path());
                }
            }
        }
        std::sort(files.begin(), files.end());

        if (files.empty()) {
            auto label = CCLabelBMFont::create("No saved macros yet", "chatFont.fnt");
            label->setScale(0.6f);
            label->setPosition({ 150, 70 });
            content->addChild(label);
            content->setContentSize({ 300, 140 });
            return;
        }

        float rowH = 28.0f;
        float totalHeight = std::max(140.0f, static_cast<float>(files.size()) * rowH);
        content->setContentSize({ 300, totalHeight });

        for (size_t i = 0; i < files.size(); i++) {
            auto& path = files[i];
            std::string name = path.filename().string();
            std::string shown = name.size() > 18 ? (name.substr(0, 18) + "..") : name;
            float y = totalHeight - static_cast<float>(i + 1) * rowH + (rowH / 2.0f);

            auto label = CCLabelBMFont::create(shown.c_str(), "chatFont.fnt");
            label->setScale(0.5f);
            label->setAnchorPoint({ 0.0f, 0.5f });
            label->setPosition({ 12, y });
            content->addChild(label);

            auto loadSpr = ButtonSprite::create("LOAD");
            loadSpr->setScale(0.45f);
            auto loadBtn = CCMenuItemSpriteExtra::create(loadSpr, this, menu_selector(BotPauseLayer::onLoadFile));
            loadBtn->setPosition({ 220, y });
            auto pathStr = CCString::create(path.string());
            loadBtn->setUserObject(pathStr);
            listMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("X");
            delSpr->setScale(0.45f);
            auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(BotPauseLayer::onDeleteFile));
            delBtn->setPosition({ 270, y });
            auto pathStr2 = CCString::create(path.string());
            delBtn->setUserObject(pathStr2);
            listMenu->addChild(delBtn);
        }

        scroll->moveToTop();
    }

    void onOpenMacroPanel(CCObject*) {
        m_fields->m_macroPanel->setVisible(true);
        m_fields->m_mainBotMenu->setVisible(false);
        this->refreshMacroList();
    }

    void onCloseMacroPanel(CCObject*) {
        m_fields->m_macroPanel->setVisible(false);
        m_fields->m_mainBotMenu->setVisible(true);
        this->updateBotLabel();
    }

    void onSaveCurrentMacro(CCObject*) {
        if (bot::g.macro.empty()) {
            bot::notify("Nothing to save", NotificationIcon::Warning);
            return;
        }

        auto now = std::time(nullptr);
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%m%d_%H%M%S", std::localtime(&now));

        std::string filename = fmt::format("{}_{}{}", bot::g.levelName, timeBuf, bot::kMacroExt);
        if (bot::writeMacroToFile(bot::macroPath(filename), bot::g.macro)) {
            bot::notify("Saved as " + filename, NotificationIcon::Success);
            this->refreshMacroList();
        } else {
            bot::notify("Save failed", NotificationIcon::Error);
        }
    }

    void onLoadFile(CCObject* sender) {
        auto item = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto pathStr = static_cast<CCString*>(item->getUserObject());
        if (!pathStr) return;

        std::vector<bot::Action> macro;
        if (bot::readMacroFromFile(pathStr->getCString(), macro)) {
            bot::g.macro = std::move(macro);
            bot::g.index = 0;
            bot::notify("Macro loaded", NotificationIcon::Success);
            this->updateBotLabel();
        } else {
            bot::notify("Failed to load macro", NotificationIcon::Error);
        }
    }

    void onDeleteFile(CCObject* sender) {
        auto item = static_cast<CCMenuItemSpriteExtra*>(sender);
        auto pathStr = static_cast<CCString*>(item->getUserObject());
        if (!pathStr) return;

        std::error_code ec;
        std::filesystem::remove(pathStr->getCString(), ec);
        this->refreshMacroList();
    }

    void updateBotLabel() {
        auto label = static_cast<CCLabelBMFont*>(this->getChildByID("bot-status"));
        if (label) {
            label->setString(bot::statusText().c_str());
        }
    }

    void onSpeedDown(CCObject*) {
        bot::recSpeed -= 0.1;
        if (bot::recSpeed < 0.05) bot::recSpeed = 0.05;
        (void)Mod::get()->setSettingValue<double>("record-speed", bot::recSpeed);
        this->updateBotLabel();
    }

    void onSpeedUp(CCObject*) {
        bot::recSpeed += 0.1;
        if (bot::recSpeed > 1.0) bot::recSpeed = 1.0;
        (void)Mod::get()->setSettingValue<double>("record-speed", bot::recSpeed);
        this->updateBotLabel();
    }

    void onBotRec(CCObject*) {
        bot::g.mode = bot::Mode::Record;
        bot::g.macro.clear();
        bot::g.step = 0;
        bot::g.index = 0;
        bot::applyClickFlags(PlayLayer::get());
        this->updateBotLabel();
        this->onRestart(nullptr);
    }

    void onBotPlay(CCObject*) {
        if (bot::g.macro.empty()) {
            bot::notify("No macro to play", NotificationIcon::Warning);
            return;
        }
        bot::g.mode = bot::Mode::Play;
        bot::g.step = 0;
        bot::g.index = 0;
        bot::applyClickFlags(PlayLayer::get());
        this->updateBotLabel();
        this->onRestart(nullptr);
    }

    void onBotStop(CCObject*) {
        bot::g.mode = bot::Mode::Off;
        bot::applyClickFlags(PlayLayer::get());
        this->updateBotLabel();
    }

    void onBotClear(CCObject*) {
        bot::g.macro.clear();
        bot::g.step = 0;
        bot::g.index = 0;
        this->updateBotLabel();
    }
};

$on_mod(Loaded) {
    bot::recSpeed = Mod::get()->getSettingValue<double>("record-speed");
}
