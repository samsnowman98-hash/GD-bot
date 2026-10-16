#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <chrono>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <filesystem>

using namespace geode::prelude;

namespace bot {
    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t step = 0;
        double frac = 0.0;
        bool down = false;
        bool p1 = true;
        uint8_t btn = 1;
    };

    struct CheckpointData {
        uint32_t step = 0;
        size_t macroSize = 0;
        CCPoint p1Pos = CCPointZero;
        double p1YVel = 0.0;
        float p1Rot = 0.0f;
        bool p1OnGround = false;
        CCPoint p2Pos = CCPointZero;
        double p2YVel = 0.0;
        float p2Rot = 0.0f;
        bool p2OnGround = false;
    };

    struct ButtonState {
        bool jump = false;
        bool left = false;
        bool right = false;

        bool isHeld(int btn) const {
            if (btn == 1) return jump;
            if (btn == 2) return left;
            if (btn == 3) return right;
            return false;
        }

        void setHeld(int btn, bool held) {
            if (btn == 1) jump = held;
            else if (btn == 2) left = held;
            else if (btn == 3) right = held;
        }
    };

    inline Mode mode = Mode::Off;
    inline std::vector<Action> currentMacro;
    inline size_t macroIndex = 0;
    inline uint32_t currentStep = 0;
    inline double recSpeed = 1.0;
    inline std::chrono::steady_clock::time_point stepStartTime;
    inline std::unordered_map<CheckpointObject*, CheckpointData> checkpoints;
    inline std::string currentMacroName = "autosave";
    inline ButtonState p1State;
    inline ButtonState p2State;
    inline bool isSubStepping = false;
    inline bool dualUpdatedInP1 = false;
    inline bool suppressRecording = false;

    inline constexpr uint32_t MAGIC = 0x32544F42;
    inline constexpr double SMALLEST = 1e-5;

    inline void resetTimingBaseline() {
        stepStartTime = std::chrono::steady_clock::now();
    }

    inline std::filesystem::path getMacrosDir() {
        auto dir = Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    inline bool saveToFile(const std::string& name) {
        auto path = getMacrosDir() / (name + ".gdbot");
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        uint32_t magic = MAGIC;
        uint32_t count = static_cast<uint32_t>(currentMacro.size());
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));

        for (const auto& act : currentMacro) {
            file.write(reinterpret_cast<const char*>(&act.step), sizeof(act.step));
            file.write(reinterpret_cast<const char*>(&act.frac), sizeof(act.frac));
            uint8_t flags = (act.down ? 1 : 0) | (act.p1 ? 2 : 0);
            file.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
            file.write(reinterpret_cast<const char*>(&act.btn), sizeof(act.btn));
        }
        return true;
    }

    inline bool loadFromFile(const std::string& name) {
        auto path = getMacrosDir() / (name + ".gdbot");
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        uint32_t magic = 0;
        uint32_t count = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != MAGIC) return false;

        file.read(reinterpret_cast<char*>(&count), sizeof(count));
        currentMacro.clear();
        currentMacro.reserve(count);

        for (uint32_t i = 0; i < count; i++) {
            Action act;
            file.read(reinterpret_cast<char*>(&act.step), sizeof(act.step));
            file.read(reinterpret_cast<char*>(&act.frac), sizeof(act.frac));
            uint8_t flags = 0;
            file.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            file.read(reinterpret_cast<char*>(&act.btn), sizeof(act.btn));
            act.down = (flags & 1) != 0;
            act.p1 = (flags & 2) != 0;
            currentMacro.push_back(act);
        }
        macroIndex = 0;
        currentStep = 0;
        currentMacroName = name;
        return true;
    }

    inline std::vector<std::string> listMacros() {
        std::vector<std::string> list;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(getMacrosDir(), ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".gdbot") {
                list.push_back(entry.path().stem().string());
            }
        }
        std::sort(list.begin(), list.end());
        return list;
    }

    inline void syncPlayback() {
        macroIndex = 0;
        while (macroIndex < currentMacro.size() && currentMacro[macroIndex].step < currentStep) {
            macroIndex++;
        }
    }

    inline void applyActionDirect(const Action& act, PlayLayer* pl) {
        PlayerObject* target = act.p1 ? pl->m_player1 : pl->m_player2;
        if (!target) return;
        PlayerButton btn = static_cast<PlayerButton>(act.btn);
        if (act.down) {
            target->pushButton(btn);
        } else {
            target->releaseButton(btn);
        }
    }

    inline void recordAction(bool isPlayer1, PlayerButton button, bool down) {
        if (mode != Mode::Record) return;
        if (suppressRecording) return;

        auto& state = isPlayer1 ? p1State : p2State;
        int btnId = static_cast<int>(button);
        if (state.isHeld(btnId) == down) {
            return;
        }
        state.setHeld(btnId, down);

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - stepStartTime).count();
        double stepDuration = (1.0 / 240.0) / (recSpeed > SMALLEST ? recSpeed : 1.0);
        double frac = std::clamp(elapsed / stepDuration, 0.0, 0.999);

        Action act;
        act.step = currentStep;
        act.frac = frac;
        act.down = down;
        act.p1 = isPlayer1;
        act.btn = static_cast<uint8_t>(btnId);
        currentMacro.push_back(act);
    }
}

struct BotPlayLayer : Modify<BotPlayLayer, PlayLayer> {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        this->m_clickBetweenSteps = false;
        bot::currentStep = 0;
        bot::macroIndex = 0;
        bot::checkpoints.clear();
        bot::p1State = bot::ButtonState();
        bot::p2State = bot::ButtonState();
        bot::isSubStepping = false;
        bot::dualUpdatedInP1 = false;
        bot::suppressRecording = false;
        bot::recSpeed = Mod::get()->getSettingValue<double>("record-speed");
        if (bot::recSpeed < 0.01) bot::recSpeed = 1.0;
        bot::resetTimingBaseline();

        if (bot::mode == bot::Mode::Record) {
            CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
        } else {
            CCScheduler::get()->setTimeScale(1.0f);
        }
        return true;
    }

    void resetLevel() {
        this->m_clickBetweenSteps = false;
        PlayLayer::resetLevel();

        bot::currentStep = 0;
        bot::macroIndex = 0;
        bot::isSubStepping = false;
        bot::dualUpdatedInP1 = false;
        bot::resetTimingBaseline();

        bot::suppressRecording = true;
        if (this->m_player1) {
            this->m_player1->releaseButton(PlayerButton::Jump);
            this->m_player1->releaseButton(PlayerButton::Left);
            this->m_player1->releaseButton(PlayerButton::Right);
        }
        if (this->m_player2) {
            this->m_player2->releaseButton(PlayerButton::Jump);
            this->m_player2->releaseButton(PlayerButton::Left);
            this->m_player2->releaseButton(PlayerButton::Right);
        }
        bot::suppressRecording = false;

        bot::p1State = bot::ButtonState();
        bot::p2State = bot::ButtonState();

        if (bot::mode == bot::Mode::Record) {
            bot::currentMacro.clear();
            CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
        } else {
            CCScheduler::get()->setTimeScale(1.0f);
        }
    }

    void handleButton(bool isDown, int button, bool isPlayer1) {
        if (bot::mode == bot::Mode::Play) {
            return;
        }
        PlayLayer::handleButton(isDown, button, isPlayer1);
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (!cp) return cp;

        bot::CheckpointData data;
        data.step = bot::currentStep;
        data.macroSize = bot::currentMacro.size();

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

        bot::checkpoints[cp] = data;
        return cp;
    }

    void removeCheckpoint(bool p0) {
        PlayLayer::removeCheckpoint(p0);
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        if (!cp) return;

        auto it = bot::checkpoints.find(cp);
        if (it == bot::checkpoints.end()) return;

        const auto& data = it->second;
        bot::currentStep = data.step;

        if (bot::mode == bot::Mode::Record) {
            if (data.macroSize <= bot::currentMacro.size()) {
                bot::currentMacro.resize(data.macroSize);
            }
        } else if (bot::mode == bot::Mode::Play) {
            bot::syncPlayback();
        }

        bot::suppressRecording = true;

        if (this->m_player1) {
            this->m_player1->setPosition(data.p1Pos);
            this->m_player1->m_yVelocity = data.p1YVel;
            this->m_player1->setRotation(data.p1Rot);
            this->m_player1->m_isOnGround = data.p1OnGround;
            this->m_player1->releaseButton(PlayerButton::Jump);
            this->m_player1->releaseButton(PlayerButton::Left);
            this->m_player1->releaseButton(PlayerButton::Right);
        }

        if (this->m_player2) {
            this->m_player2->setPosition(data.p2Pos);
            this->m_player2->m_yVelocity = data.p2YVel;
            this->m_player2->setRotation(data.p2Rot);
            this->m_player2->m_isOnGround = data.p2OnGround;
            this->m_player2->releaseButton(PlayerButton::Jump);
            this->m_player2->releaseButton(PlayerButton::Left);
            this->m_player2->releaseButton(PlayerButton::Right);
        }

        bot::suppressRecording = false;

        bot::p1State = bot::ButtonState();
        bot::p2State = bot::ButtonState();
        bot::isSubStepping = false;
        bot::dualUpdatedInP1 = false;
        bot::resetTimingBaseline();
    }
};

struct BotPlayerObject : Modify<BotPlayerObject, PlayerObject> {
    void pushButton(PlayerButton button) {
        PlayerObject::pushButton(button);

        auto pl = PlayLayer::get();
        if (!pl) return;
        if (this != pl->m_player1 && this != pl->m_player2) return;

        bool isPlayer1 = (this == pl->m_player1);
        bot::recordAction(isPlayer1, button, true);
    }

    void releaseButton(PlayerButton button) {
        PlayerObject::releaseButton(button);

        auto pl = PlayLayer::get();
        if (!pl) return;
        if (this != pl->m_player1 && this != pl->m_player2) return;

        bool isPlayer1 = (this == pl->m_player1);
        bot::recordAction(isPlayer1, button, false);
    }

    void update(float dt) {
        auto pl = PlayLayer::get();
        if (!pl || pl->m_isPaused || bot::isSubStepping) {
            PlayerObject::update(dt);
            return;
        }

        if (this == pl->m_player2 && bot::dualUpdatedInP1) {
            bot::dualUpdatedInP1 = false;
            return;
        }

        if (this != pl->m_player1) {
            PlayerObject::update(dt);
            return;
        }

        bool isDual = (pl->m_player2 != nullptr && pl->m_player2->isVisible());
        bot::dualUpdatedInP1 = isDual;
        bot::stepStartTime = std::chrono::steady_clock::now();

        if (bot::mode == bot::Mode::Play) {
            double currentFrac = 0.0;
            bot::isSubStepping = true;

            while (bot::macroIndex < bot::currentMacro.size()) {
                const auto& act = bot::currentMacro[bot::macroIndex];
                if (act.step != bot::currentStep) {
                    break;
                }

                double targetFrac = std::clamp(act.frac, 0.0, 0.999);
                if (targetFrac > currentFrac) {
                    float subDelta = static_cast<float>((targetFrac - currentFrac) * dt);
                    PlayerObject::update(subDelta);
                    if (isDual) {
                        pl->m_player2->update(subDelta);
                    }
                    currentFrac = targetFrac;
                }

                bot::applyActionDirect(act, pl);
                bot::macroIndex++;
            }

            float remDelta = static_cast<float>((1.0 - currentFrac) * dt);
            PlayerObject::update(remDelta);
            if (isDual) {
                pl->m_player2->update(remDelta);
            }

            bot::isSubStepping = false;
            bot::currentStep++;
            return;
        }

        PlayerObject::update(dt);
        bot::currentStep++;
    }
};

struct BotPauseLayer : Modify<BotPauseLayer, PauseLayer> {
    struct Fields {
        CCLabelBMFont* statusLabel = nullptr;
        CCLabelBMFont* speedLabel = nullptr;
        CCMenu* botMenu = nullptr;
        CCLayerColor* macroOverlay = nullptr;
        ScrollLayer* macroScroll = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        m_fields->botMenu = CCMenu::create();
        m_fields->botMenu->setID("tas-bot-panel"_spr);
        m_fields->botMenu->setPosition(winSize.width / 2.0f, 42.0f);
        m_fields->botMenu->setContentSize({winSize.width - 20.0f, 32.0f});

        auto layout = RowLayout::create();
        layout->setGap(6.0f);
        layout->setAxisAlignment(AxisAlignment::Center);
        m_fields->botMenu->setLayout(layout);

        auto recSpr = ButtonSprite::create("REC", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        recSpr->setScale(0.55f);
        auto recBtn = CCMenuItemSpriteExtra::create(recSpr, this, menu_selector(BotPauseLayer::onRecord));

        auto playSpr = ButtonSprite::create("PLAY", "goldFont.fnt", "GJ_button_02.png", 0.7f);
        playSpr->setScale(0.55f);
        auto playBtn = CCMenuItemSpriteExtra::create(playSpr, this, menu_selector(BotPauseLayer::onPlay));

        auto stopSpr = ButtonSprite::create("OFF", "goldFont.fnt", "GJ_button_06.png", 0.7f);
        stopSpr->setScale(0.55f);
        auto stopBtn = CCMenuItemSpriteExtra::create(stopSpr, this, menu_selector(BotPauseLayer::onStop));

        auto clearSpr = ButtonSprite::create("CLR", "goldFont.fnt", "GJ_button_06.png", 0.7f);
        clearSpr->setScale(0.55f);
        auto clearBtn = CCMenuItemSpriteExtra::create(clearSpr, this, menu_selector(BotPauseLayer::onClear));

        auto macrosSpr = ButtonSprite::create("FILES", "goldFont.fnt", "GJ_button_04.png", 0.7f);
        macrosSpr->setScale(0.55f);
        auto macrosBtn = CCMenuItemSpriteExtra::create(macrosSpr, this, menu_selector(BotPauseLayer::onMacros));

        auto minusSpr = ButtonSprite::create("-", "bigFont.fnt", "GJ_button_01.png", 0.7f);
        minusSpr->setScale(0.55f);
        auto minusBtn = CCMenuItemSpriteExtra::create(minusSpr, this, menu_selector(BotPauseLayer::onSpeedMinus));

        m_fields->speedLabel = CCLabelBMFont::create(fmt::format("{:.2f}x", bot::recSpeed).c_str(), "bigFont.fnt");
        m_fields->speedLabel->setScale(0.35f);

        auto plusSpr = ButtonSprite::create("+", "bigFont.fnt", "GJ_button_01.png", 0.7f);
        plusSpr->setScale(0.55f);
        auto plusBtn = CCMenuItemSpriteExtra::create(plusSpr, this, menu_selector(BotPauseLayer::onSpeedPlus));

        m_fields->botMenu->addChild(recBtn);
        m_fields->botMenu->addChild(playBtn);
        m_fields->botMenu->addChild(stopBtn);
        m_fields->botMenu->addChild(clearBtn);
        m_fields->botMenu->addChild(macrosBtn);
        m_fields->botMenu->addChild(minusBtn);
        m_fields->botMenu->addChild(m_fields->speedLabel);
        m_fields->botMenu->addChild(plusBtn);

        m_fields->botMenu->updateLayout();
        this->addChild(m_fields->botMenu, 100);

        m_fields->statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_fields->statusLabel->setPosition(winSize.width / 2.0f, 18.0f);
        m_fields->statusLabel->setScale(0.55f);
        this->addChild(m_fields->statusLabel, 100);

        updateLabels();
    }

    void updateLabels() {
        if (!m_fields->statusLabel) return;

        std::string modeStr = "OFF";
        ccColor3B col = {180, 180, 180};
        if (bot::mode == bot::Mode::Record) {
            modeStr = "REC";
            col = {255, 60, 60};
        } else if (bot::mode == bot::Mode::Play) {
            modeStr = "PLAY";
            col = {60, 255, 60};
        }

        m_fields->statusLabel->setString(
            fmt::format("MODE: {} | ACTS: {} | STEP: {} | FILE: {}",
                modeStr, bot::currentMacro.size(), bot::currentStep, bot::currentMacroName).c_str()
        );
        m_fields->statusLabel->setColor(col);

        if (m_fields->speedLabel) {
            m_fields->speedLabel->setString(fmt::format("{:.2f}x", bot::recSpeed).c_str());
        }
    }

    void onRecord(CCObject*) {
        bot::mode = bot::Mode::Record;
        bot::currentMacro.clear();
        bot::currentStep = 0;
        bot::macroIndex = 0;
        bot::p1State = bot::ButtonState();
        bot::p2State = bot::ButtonState();
        CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
        updateLabels();
    }

    void onPlay(CCObject*) {
        bot::mode = bot::Mode::Play;
        bot::macroIndex = 0;
        bot::currentStep = 0;
        bot::p1State = bot::ButtonState();
        bot::p2State = bot::ButtonState();
        CCScheduler::get()->setTimeScale(1.0f);
        updateLabels();
    }

    void onStop(CCObject*) {
        bot::mode = bot::Mode::Off;
        CCScheduler::get()->setTimeScale(1.0f);
        updateLabels();
    }

    void onClear(CCObject*) {
        bot::currentMacro.clear();
        bot::macroIndex = 0;
        bot::currentStep = 0;
        updateLabels();
    }

    void onSpeedMinus(CCObject*) {
        bot::recSpeed = std::max(0.1, bot::recSpeed - 0.1);
        Mod::get()->setSettingValue<double>("record-speed", bot::recSpeed);
        if (bot::mode == bot::Mode::Record) {
            CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
        }
        updateLabels();
    }

    void onSpeedPlus(CCObject*) {
        bot::recSpeed = std::min(1.0, bot::recSpeed + 0.1);
        Mod::get()->setSettingValue<double>("record-speed", bot::recSpeed);
        if (bot::mode == bot::Mode::Record) {
            CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
        }
        updateLabels();
    }

    void onMacros(CCObject*) {
        buildMacroBrowser();
    }

    void buildMacroBrowser() {
        if (m_fields->macroOverlay) {
            m_fields->macroOverlay->removeFromParentAndCleanup(true);
            m_fields->macroOverlay = nullptr;
        }

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        m_fields->macroOverlay = CCLayerColor::create({0, 0, 0, 220});
        m_fields->macroOverlay->setContentSize(winSize);
        m_fields->macroOverlay->setTouchPriority(-500);

        auto panel = CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({340.0f, 220.0f});
        panel->setPosition(winSize / 2.0f);
        m_fields->macroOverlay->addChild(panel);

        auto title = CCLabelBMFont::create("MACROS", "bigFont.fnt");
        title->setScale(0.55f);
        title->setPosition(winSize.width / 2.0f, winSize.height / 2.0f + 92.0f);
        m_fields->macroOverlay->addChild(title);

        auto topMenu = CCMenu::create();
        topMenu->setPosition(CCPointZero);
        topMenu->setTouchPriority(-501);

        auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        closeSpr->setScale(0.7f);
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSpr, this, menu_selector(BotPauseLayer::onCloseBrowser));
        closeBtn->setPosition(winSize.width / 2.0f + 155.0f, winSize.height / 2.0f + 95.0f);
        topMenu->addChild(closeBtn);

        auto saveSpr = ButtonSprite::create("SAVE CURRENT", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        saveSpr->setScale(0.6f);
        auto saveBtn = CCMenuItemSpriteExtra::create(saveSpr, this, menu_selector(BotPauseLayer::onSaveNewMacro));
        saveBtn->setPosition(winSize.width / 2.0f, winSize.height / 2.0f - 88.0f);
        topMenu->addChild(saveBtn);

        m_fields->macroOverlay->addChild(topMenu);

        auto list = bot::listMacros();
        float itemH = 28.0f;
        float contentH = std::max(130.0f, static_cast<float>(list.size()) * itemH);

        auto scroll = ScrollLayer::create({300.0f, 130.0f});
        scroll->setPosition(winSize.width / 2.0f - 150.0f, winSize.height / 2.0f - 65.0f);
        scroll->m_contentLayer->setContentSize({300.0f, contentH});

        auto rowMenu = CCMenu::create();
        rowMenu->setPosition(CCPointZero);
        rowMenu->setContentSize({300.0f, contentH});
        rowMenu->setTouchPriority(-501);

        float y = contentH - (itemH / 2.0f);
        for (const auto& name : list) {
            auto lbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
            lbl->setScale(0.55f);
            lbl->setAnchorPoint({0.0f, 0.5f});
            lbl->setPosition(10.0f, y);
            rowMenu->addChild(lbl);

            auto loadSpr = ButtonSprite::create("LOAD", "goldFont.fnt", "GJ_button_02.png", 0.5f);
            loadSpr->setScale(0.5f);
            auto loadBtn = CCMenuItemSpriteExtra::create(loadSpr, this, menu_selector(BotPauseLayer::onLoadItem));
            loadBtn->setUserObject(CCString::create(name));
            loadBtn->setPosition(210.0f, y);
            rowMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("DEL", "goldFont.fnt", "GJ_button_06.png", 0.5f);
            delSpr->setScale(0.5f);
            auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(BotPauseLayer::onDeleteItem));
            delBtn->setUserObject(CCString::create(name));
            delBtn->setPosition(260.0f, y);
            rowMenu->addChild(delBtn);

            y -= itemH;
        }

        scroll->m_contentLayer->addChild(rowMenu);
        scroll->moveToTop();
        m_fields->macroOverlay->addChild(scroll);
        this->addChild(m_fields->macroOverlay, 200);
    }

    void onCloseBrowser(CCObject*) {
        if (m_fields->macroOverlay) {
            m_fields->macroOverlay->removeFromParentAndCleanup(true);
            m_fields->macroOverlay = nullptr;
        }
        updateLabels();
    }

    void onSaveNewMacro(CCObject*) {
        auto t = std::time(nullptr);
        auto tm = *std::localtime(&t);
        std::ostringstream oss;
        oss << "macro_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
        std::string name = oss.str();
        bot::saveToFile(name);
        bot::currentMacroName = name;
        buildMacroBrowser();
    }

    void onLoadItem(CCObject* sender) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        if (!btn) return;
        auto str = static_cast<CCString*>(btn->getUserObject());
        if (!str) return;

        bot::loadFromFile(str->getCString());
        onCloseBrowser(nullptr);
    }

    void onDeleteItem(CCObject* sender) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        if (!btn) return;
        auto str = static_cast<CCString*>(btn->getUserObject());
        if (!str) return;

        auto path = bot::getMacrosDir() / (std::string(str->getCString()) + ".gdbot");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        buildMacroBrowser();
    }
};
