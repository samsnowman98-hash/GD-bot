#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Popup.hpp>

#include <chrono>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <filesystem>
#include <ctime>
#include <cstdint>

using namespace geode::prelude;

// ===========================================================================
//  bot namespace
// ===========================================================================
namespace bot {
    enum class Mode { Off, Record, Play };

    struct Action {
        double time = 0.0;
        bool down = false;
        bool p1 = true;
        uint8_t btn = 1;
    };

    struct CheckpointData {
        double time = 0.0;
        size_t macroSize = 0;
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
        void reset() { jump = left = right = false; }
    };

    inline Mode mode = Mode::Off;
    inline std::vector<Action> currentMacro;
    inline size_t macroIndex = 0;
    inline std::string currentMacroName = "autosave";

    inline double gameTime = 0.0;
    inline double frameStartGameTime = 0.0;
    inline float currentFrameDt = 1.0f / 60.0f;
    inline std::chrono::steady_clock::time_point frameStartReal;

    inline double recSpeed = 1.0;
    inline ButtonState p1State;
    inline ButtonState p2State;

    // True while we synthesize input from playback so the record hook ignores it.
    inline bool internalInput = false;

    // Bumped on every reset / checkpoint load; substep loop uses it to bail out.
    inline uint32_t resetCounter = 0;

    inline std::unordered_map<CheckpointObject*, CheckpointData> checkpoints;

    inline constexpr uint32_t MAGIC = 0x34544F42;
    inline constexpr const char* FILE_EXT = ".gdbot";

    inline std::filesystem::path getMacrosDir() {
        auto dir = Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    inline void resetTiming() {
        frameStartReal = std::chrono::steady_clock::now();
        frameStartGameTime = gameTime;
    }

    inline double parseSpeedString(const std::string& str) {
        if (str.empty()) return -1.0;
        try {
            size_t pos = 0;
            double v = std::stod(str, &pos);
            if (pos == 0) return -1.0;
            return v;
        } catch (...) {
            return -1.0;
        }
    }

    inline bool saveToFile(const std::string& name) {
        auto path = getMacrosDir() / (name + FILE_EXT);
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        uint32_t magic = MAGIC;
        uint32_t count = static_cast<uint32_t>(currentMacro.size());
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));

        for (const auto& act : currentMacro) {
            file.write(reinterpret_cast<const char*>(&act.time), sizeof(act.time));
            uint8_t flags = (act.down ? 1 : 0) | (act.p1 ? 2 : 0);
            file.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
            file.write(reinterpret_cast<const char*>(&act.btn), sizeof(act.btn));
        }
        file.flush();
        return file.good();
    }

    inline bool loadFromFile(const std::string& name) {
        auto path = getMacrosDir() / (name + FILE_EXT);
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        uint32_t magic = 0, count = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (!file.good() || magic != MAGIC) return false;

        file.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!file.good()) return false;
        if (count > 20'000'000u) return false;

        std::vector<Action> loaded;
        loaded.reserve(count);
        for (uint32_t i = 0; i < count; i++) {
            Action act;
            uint8_t flags = 0;
            file.read(reinterpret_cast<char*>(&act.time), sizeof(act.time));
            file.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            file.read(reinterpret_cast<char*>(&act.btn), sizeof(act.btn));
            if (!file.good()) return false;
            act.down = (flags & 1) != 0;
            act.p1 = (flags & 2) != 0;
            loaded.push_back(act);
        }

        std::stable_sort(loaded.begin(), loaded.end(),
            [](const Action& a, const Action& b) { return a.time < b.time; });

        currentMacro = std::move(loaded);
        macroIndex = 0;
        gameTime = 0.0;
        frameStartGameTime = 0.0;
        currentMacroName = name;
        return true;
    }

    inline std::vector<std::string> listMacros() {
        std::vector<std::string> list;
        std::error_code ec;
        auto dir = getMacrosDir();
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec)) {
            const auto& entry = *it;
            std::error_code e2;
            if (entry.is_regular_file(e2) && entry.path().extension() == FILE_EXT) {
                list.push_back(entry.path().stem().string());
            }
        }
        std::sort(list.begin(), list.end());
        return list;
    }

    inline void syncPlaybackIndex() {
        macroIndex = 0;
        while (macroIndex < currentMacro.size() &&
               currentMacro[macroIndex].time < gameTime) {
            macroIndex++;
        }
    }

    inline void deriveHolds(size_t upTo, ButtonState& outP1, ButtonState& outP2) {
        outP1.reset();
        outP2.reset();
        size_t n = std::min(upTo, currentMacro.size());
        for (size_t i = 0; i < n; i++) {
            const auto& act = currentMacro[i];
            (act.p1 ? outP1 : outP2).setHeld(act.btn, act.down);
        }
    }
}

// ===========================================================================
//  Input hook — record user input, block it during playback
// ===========================================================================
struct BotBaseGameLayer : Modify<BotBaseGameLayer, GJBaseGameLayer> {
    void handleButton(bool push, int button, bool player1) {
        if (!bot::internalInput) {
            if (bot::mode == bot::Mode::Play) {
                // Block user input while playback is still active.
                if (bot::macroIndex < bot::currentMacro.size()) return;
            } else if (bot::mode == bot::Mode::Record) {
                auto pl = PlayLayer::get();
                if (pl && static_cast<GJBaseGameLayer*>(pl) == this && !pl->m_isPaused) {
                    auto now = std::chrono::steady_clock::now();
                    double elapsedReal = std::chrono::duration<double>(
                        now - bot::frameStartReal).count();

                    double timeScale = static_cast<double>(CCScheduler::get()->getTimeScale());
                    if (timeScale <= 1e-6) timeScale = 1.0;

                    double frameEnd = bot::frameStartGameTime + bot::currentFrameDt;
                    double clickGameTime = bot::frameStartGameTime + elapsedReal * timeScale;
                    clickGameTime = std::clamp(clickGameTime,
                                               bot::frameStartGameTime, frameEnd);

                    auto& state = player1 ? bot::p1State : bot::p2State;
                    if (state.isHeld(button) != push) {
                        state.setHeld(button, push);
                        bot::Action act;
                        act.time = clickGameTime;
                        act.down = push;
                        act.p1 = player1;
                        act.btn = static_cast<uint8_t>(button);
                        bot::currentMacro.push_back(act);
                    }
                }
            }
        }

        GJBaseGameLayer::handleButton(push, button, player1);
    }
};

// ===========================================================================
//  PlayLayer hook — timing, substep playback, checkpoints
// ===========================================================================
struct BotPlayLayer : Modify<BotPlayLayer, PlayLayer> {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        this->m_clickBetweenSteps = false;

        bot::gameTime = 0.0;
        bot::frameStartGameTime = 0.0;
        bot::macroIndex = 0;
        bot::checkpoints.clear();
        bot::p1State.reset();
        bot::p2State.reset();
        bot::internalInput = false;
        bot::resetCounter = 0;

        bot::recSpeed = std::clamp(
            Mod::get()->getSettingValue<double>("record-speed"), 0.01, 2.0);
        bot::resetTiming();

        CCScheduler::get()->setTimeScale(
            bot::mode == bot::Mode::Record ? static_cast<float>(bot::recSpeed) : 1.0f);
        return true;
    }

    void update(float dt) {
        if (this->m_isPaused) {
            PlayLayer::update(dt);
            return;
        }

        if (bot::mode == bot::Mode::Play) {
            double frameStart = bot::gameTime;
            double frameEnd = frameStart + static_cast<double>(dt);
            double currentT = frameStart;
            uint32_t myReset = bot::resetCounter;

            while (bot::macroIndex < bot::currentMacro.size()) {
                const auto& act = bot::currentMacro[bot::macroIndex];
                if (act.time >= frameEnd) break;

                double actTime = std::clamp(act.time, currentT, frameEnd);
                float subDelta = static_cast<float>(actTime - currentT);
                if (subDelta > 1e-6f) {
                    PlayLayer::update(subDelta);
                    currentT = actTime;
                    if (bot::resetCounter != myReset) return;
                }

                bot::internalInput = true;
                this->handleButton(act.down, act.btn, act.p1);
                bot::internalInput = false;
                bot::macroIndex++;

                if (bot::resetCounter != myReset) return;
            }

            float remDelta = static_cast<float>(frameEnd - currentT);
            if (remDelta > 1e-6f) {
                PlayLayer::update(remDelta);
                if (bot::resetCounter != myReset) return;
            }

            bot::gameTime = frameEnd;
            return;
        }

        // Record / Off — single step, capture real time reference for this frame.
        bot::frameStartReal = std::chrono::steady_clock::now();
        bot::frameStartGameTime = bot::gameTime;
        bot::currentFrameDt = dt;

        PlayLayer::update(dt);

        bot::gameTime += static_cast<double>(dt);
    }

    void resetLevel() override {
        PlayLayer::resetLevel();

        this->m_clickBetweenSteps = false;

        // Full restart in Record → autosave the attempt before wiping.
        if (bot::mode == bot::Mode::Record && !bot::currentMacro.empty()) {
            bot::saveToFile("autosave");
        }

        bot::gameTime = 0.0;
        bot::frameStartGameTime = 0.0;
        bot::macroIndex = 0;
        bot::p1State.reset();
        bot::p2State.reset();
        bot::resetCounter++;

        if (bot::mode == bot::Mode::Record) {
            bot::currentMacro.clear();
        }

        CCScheduler::get()->setTimeScale(
            bot::mode == bot::Mode::Record ? static_cast<float>(bot::recSpeed) : 1.0f);

        bot::resetTiming();
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (!cp) return cp;

        bot::CheckpointData data;
        data.time = bot::gameTime;
        data.macroSize = bot::currentMacro.size();
        bot::checkpoints[cp] = data;
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        if (!cp) return;

        auto it = bot::checkpoints.find(cp);
        bool haveData = (it != bot::checkpoints.end());
        bot::CheckpointData data;
        if (haveData) data = it->second;

        PlayLayer::loadFromCheckpoint(cp);

        if (!haveData) return;

        bot::gameTime = data.time;
        bot::frameStartGameTime = data.time;

        if (bot::mode == bot::Mode::Record) {
            if (data.macroSize <= bot::currentMacro.size()) {
                bot::currentMacro.resize(data.macroSize);
            }
        } else if (bot::mode == bot::Mode::Play) {
            bot::syncPlaybackIndex();
        }

        size_t holdsUpTo = (bot::mode == bot::Mode::Play)
            ? bot::macroIndex
            : bot::currentMacro.size();
        bot::deriveHolds(holdsUpTo, bot::p1State, bot::p2State);
        applyHeldStateToGame();

        bot::resetCounter++;
        bot::resetTiming();
    }

private:
    // Push/release player buttons to match ButtonState.
    // NB: don't touch m_isHolding — it doesn't exist in this GD version,
    // PlayerObject::pushButton/releaseButton is what actually matters.
    void applyHeldStateToGame() {
        auto apply = [](PlayerObject* po, const bot::ButtonState& s) {
            if (!po) return;
            po->releaseButton(PlayerButton::Jump);
            po->releaseButton(PlayerButton::Left);
            po->releaseButton(PlayerButton::Right);
            if (s.jump)  po->pushButton(PlayerButton::Jump);
            if (s.left)  po->pushButton(PlayerButton::Left);
            if (s.right) po->pushButton(PlayerButton::Right);
        };
        apply(this->m_player1, bot::p1State);
        apply(this->m_player2, bot::p2State);
    }
};

// ===========================================================================
//  Macro browser popup — Popup<> blocks input under itself natively
// ===========================================================================
class MacroBrowserPopup : public Popup<> {
protected:
    bool setup() override {
        this->setTitle("MACROS");

        auto winSize = m_mainLayer->getContentSize();

        // Save current button
        auto saveSpr = ButtonSprite::create(
            "SAVE CURRENT", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        saveSpr->setScale(0.6f);
        auto saveBtn = CCMenuItemSpriteExtra::create(
            saveSpr, this, menu_selector(MacroBrowserPopup::onSaveNewMacro));

        auto btnMenu = CCMenu::create();
        btnMenu->addChild(saveBtn);
        btnMenu->setPosition({winSize.width / 2.0f, 22.0f});
        m_mainLayer->addChild(btnMenu);

        // List
        auto list = bot::listMacros();
        float itemH = 28.0f;
        float contentH = std::max(130.0f, static_cast<float>(list.size()) * itemH);

        auto scroll = ScrollLayer::create({300.0f, 130.0f});
        scroll->setPosition({winSize.width / 2.0f - 150.0f, winSize.height / 2.0f - 65.0f});
        scroll->m_contentLayer->setContentSize({300.0f, contentH});

        auto rowMenu = CCMenu::create();
        rowMenu->setPosition(CCPointZero);
        rowMenu->setContentSize({300.0f, contentH});

        float y = contentH - (itemH / 2.0f);
        for (const auto& name : list) {
            auto lbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
            lbl->setScale(0.55f);
            lbl->setAnchorPoint({0.0f, 0.5f});
            lbl->setPosition(10.0f, y);
            rowMenu->addChild(lbl);

            auto loadSpr = ButtonSprite::create(
                "LOAD", "goldFont.fnt", "GJ_button_02.png", 0.5f);
            loadSpr->setScale(0.5f);
            auto loadBtn = CCMenuItemSpriteExtra::create(
                loadSpr, this, menu_selector(MacroBrowserPopup::onLoadItem));
            loadBtn->setUserObject(CCString::create(name.c_str()));
            loadBtn->setPosition(210.0f, y);
            rowMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create(
                "DEL", "goldFont.fnt", "GJ_button_06.png", 0.5f);
            delSpr->setScale(0.5f);
            auto delBtn = CCMenuItemSpriteExtra::create(
                delSpr, this, menu_selector(MacroBrowserPopup::onDeleteItem));
            delBtn->setUserObject(CCString::create(name.c_str()));
            delBtn->setPosition(260.0f, y);
            rowMenu->addChild(delBtn);

            y -= itemH;
        }

        scroll->m_contentLayer->addChild(rowMenu);
        scroll->moveToTop();
        m_mainLayer->addChild(scroll);

        return true;
    }

    void onSaveNewMacro(CCObject*) {
        auto t = std::time(nullptr);
        auto tm = *std::localtime(&t);
        std::ostringstream oss;
        oss << "macro_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
        std::string name = oss.str();
        if (bot::saveToFile(name)) {
            bot::currentMacroName = name;
        }
        // Close; user reopens FILES to see the new entry.
        this->onClose(nullptr);
    }

    void onLoadItem(CCObject* sender) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        if (!btn) return;
        auto str = static_cast<CCString*>(btn->getUserObject());
        if (!str) return;

        bot::loadFromFile(str->getCString());
        // Loading resets timing; disarm bot so user re-arms REC/PLAY manually.
        bot::mode = bot::Mode::Off;
        CCScheduler::get()->setTimeScale(1.0f);
        this->onClose(nullptr);
    }

    void onDeleteItem(CCObject* sender) {
        auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
        if (!btn) return;
        auto str = static_cast<CCString*>(btn->getUserObject());
        if (!str) return;

        auto path = bot::getMacrosDir()
            / (std::string(str->getCString()) + bot::FILE_EXT);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        this->onClose(nullptr);
    }

public:
    static MacroBrowserPopup* create() {
        auto ret = new MacroBrowserPopup();
        if (ret && ret->initAnchored(360.f, 240.f, "GJ_square01.png")) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ===========================================================================
//  PauseLayer UI
// ===========================================================================
struct BotPauseLayer : Modify<BotPauseLayer, PauseLayer> {
    struct Fields {
        CCLabelBMFont* statusLabel = nullptr;
        TextInput* speedInput = nullptr;
        CCMenu* botMenu = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        m_fields->botMenu = CCMenu::create();
        m_fields->botMenu->setID("tas-bot-panel"_spr);
        m_fields->botMenu->setPosition(winSize.width / 2.0f, 40.0f);
        m_fields->botMenu->setContentSize({winSize.width - 20.0f, 30.0f});

        auto layout = RowLayout::create();
        layout->setGap(6.0f);
        layout->setAxisAlignment(AxisAlignment::Center);
        m_fields->botMenu->setLayout(layout);

        auto addBtn = [&](const char* text, const char* font,
                          const char* bg, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(text, font, bg, 0.7f);
            spr->setScale(0.55f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            m_fields->botMenu->addChild(btn);
        };

        addBtn("REC",   "goldFont.fnt", "GJ_button_01.png",
               menu_selector(BotPauseLayer::onRecord));
        addBtn("PLAY",  "goldFont.fnt", "GJ_button_02.png",
               menu_selector(BotPauseLayer::onPlay));
        addBtn("OFF",   "goldFont.fnt", "GJ_button_06.png",
               menu_selector(BotPauseLayer::onStop));
        addBtn("CLR",   "goldFont.fnt", "GJ_button_06.png",
               menu_selector(BotPauseLayer::onClear));
        addBtn("FILES", "goldFont.fnt", "GJ_button_04.png",
               menu_selector(BotPauseLayer::onMacros));

        auto spdLabel = CCLabelBMFont::create("SPD:", "goldFont.fnt");
        spdLabel->setScale(0.5f);
        m_fields->botMenu->addChild(spdLabel);

        m_fields->speedInput = TextInput::create(60.0f, "1.0", "bigFont.fnt");
        m_fields->speedInput->setScale(0.55f);
        m_fields->speedInput->setFilter("0123456789.");
        m_fields->speedInput->setMaxCharCount(6);
        m_fields->speedInput->setString(fmt::format("{:.2f}", bot::recSpeed));
        m_fields->speedInput->setCallback([this](std::string const& str) {
            double val = bot::parseSpeedString(str);
            if (val >= 0.01 && val <= 2.0) {
                bot::recSpeed = val;
                Mod::get()->setSettingValue<double>("record-speed", bot::recSpeed);
                if (bot::mode == bot::Mode::Record) {
                    CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
                }
            }
        });
        m_fields->botMenu->addChild(m_fields->speedInput);

        m_fields->botMenu->updateLayout();
        this->addChild(m_fields->botMenu, 100);

        m_fields->statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_fields->statusLabel->setPosition(winSize.width / 2.0f, 18.0f);
        m_fields->statusLabel->setScale(0.55f);
        this->addChild(m_fields->statusLabel, 100);

        updateLabels();

        // Refresh labels periodically so changes made inside macro popup
        // are reflected on the pause screen behind it.
        this->schedule(schedule_selector(BotPauseLayer::refreshTick), 0.25f);
    }

    void refreshTick(float) {
        updateLabels();
    }

    void updateLabels() {
        if (!m_fields->statusLabel) return;

        std::string modeStr = "OFF";
        ccColor3B col = {180, 180, 180};
        if (bot::mode == bot::Mode::Record) {
            modeStr = "REC";  col = {255, 60, 60};
        } else if (bot::mode == bot::Mode::Play) {
            modeStr = "PLAY"; col = {60, 255, 60};
        }

        m_fields->statusLabel->setString(fmt::format(
            "MODE: {} | ACTS: {} | TIME: {:.2f}s | FILE: {}",
            modeStr, bot::currentMacro.size(), bot::gameTime,
            bot::currentMacroName).c_str());
        m_fields->statusLabel->setColor(col);
    }

    void onRecord(CCObject*) {
        bot::mode = bot::Mode::Record;
        bot::currentMacro.clear();
        bot::macroIndex = 0;
        bot::gameTime = 0.0;
        bot::frameStartGameTime = 0.0;
        bot::p1State.reset();
        bot::p2State.reset();
        bot::resetTiming();
        CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
        updateLabels();
    }

    void onPlay(CCObject*) {
        if (bot::gameTime > 0.5) {
            FLAlertLayer::create(
                "Warning",
                "Reset the level before starting playback.",
                "OK"
            )->show();
            return;
        }
        bot::mode = bot::Mode::Play;
        bot::macroIndex = 0;
        bot::p1State.reset();
        bot::p2State.reset();
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
        bot::gameTime = 0.0;
        updateLabels();
    }

    void onMacros(CCObject*) {
        MacroBrowserPopup::create()->show();
    }
};
