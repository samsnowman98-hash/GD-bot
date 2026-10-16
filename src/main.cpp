#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/CCScrollLayerExt.hpp>
#include <Geode/binding/CCContentLayer.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <chrono>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <ctime>

using namespace geode::prelude;

enum class BotState {
    Off,
    Record,
    Play
};

struct Action {
    uint32_t step;
    double frac;
    bool down;
    bool p1;
    uint8_t btn;
};

struct BotMacro {
    std::vector<Action> actions;
};

struct BotCheckpoint {
    CheckpointObject* cp;
    uint32_t step;
    size_t macroActionCount;
    size_t playIndexP1;
    size_t playIndexP2;

    cocos2d::CCPoint posP1;
    double yVelP1;
    float rotP1;
    bool isOnGroundP1;
    bool isUpsideDownP1;

    cocos2d::CCPoint posP2;
    double yVelP2;
    float rotP2;
    bool isOnGroundP2;
    bool isUpsideDownP2;
};

namespace bot {
    inline BotState state = BotState::Off;
    inline BotMacro currentMacro;
    inline uint32_t currentStep = 0;
    inline std::chrono::steady_clock::time_point stepStartTime = std::chrono::steady_clock::now();
    inline size_t playIndexP1 = 0;
    inline size_t playIndexP2 = 0;
    inline bool isReplaying = false;
    inline double recSpeed = 1.0;
    inline std::vector<BotCheckpoint> checkpoints;

    inline std::filesystem::path getMacroDir() {
        auto dir = geode::Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    inline double parseSpeedString(const std::string& str) {
        if (str.empty()) return 1.0;
        std::string s = str;
        for (char& c : s) {
            if (c == ',') c = '.';
        }
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return 1.0;
        size_t end = s.find_last_not_of(" \t\r\n");
        s = s.substr(start, end - start + 1);

        double whole = 0.0;
        double frac = 0.0;
        double div = 1.0;
        bool inFrac = false;
        bool hasDigits = false;

        for (char c : s) {
            if (c >= '0' && c <= '9') {
                hasDigits = true;
                if (!inFrac) {
                    whole = whole * 10.0 + static_cast<double>(c - '0');
                } else {
                    div *= 10.0;
                    frac += static_cast<double>(c - '0') / div;
                }
            } else if (c == '.' && !inFrac) {
                inFrac = true;
            } else {
                break;
            }
        }
        if (!hasDigits) return 1.0;
        double val = whole + frac;
        if (val < 0.01) val = 0.01;
        if (val > 2.0) val = 2.0;
        return val;
    }

    inline bool saveMacro(const std::filesystem::path& path, const BotMacro& macro) {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        uint32_t magic = 0x32544F42;
        out.write(reinterpret_cast<const char*>(&magic), 4);
        uint32_t count = static_cast<uint32_t>(macro.actions.size());
        out.write(reinterpret_cast<const char*>(&count), 4);
        for (const auto& act : macro.actions) {
            out.write(reinterpret_cast<const char*>(&act.step), 4);
            out.write(reinterpret_cast<const char*>(&act.frac), 8);
            uint8_t flags = (act.down ? 1 : 0) | (act.p1 ? 2 : 0);
            out.write(reinterpret_cast<const char*>(&flags), 1);
            out.write(reinterpret_cast<const char*>(&act.btn), 1);
        }
        return true;
    }

    inline bool loadMacro(const std::filesystem::path& path, BotMacro& macro) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        uint32_t magic = 0;
        in.read(reinterpret_cast<char*>(&magic), 4);
        if (magic != 0x32544F42) return false;
        uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), 4);
        macro.actions.clear();
        macro.actions.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Action act;
            in.read(reinterpret_cast<char*>(&act.step), 4);
            in.read(reinterpret_cast<char*>(&act.frac), 8);
            uint8_t flags = 0;
            in.read(reinterpret_cast<char*>(&flags), 1);
            act.down = (flags & 1) != 0;
            act.p1 = (flags & 2) != 0;
            in.read(reinterpret_cast<char*>(&act.btn), 1);
            macro.actions.push_back(act);
        }
        return true;
    }
}

static void stepPlayerWithCBF(PlayerObject* player, bool isP1, float dt, PlayLayer* pl) {
    size_t& idx = isP1 ? bot::playIndexP1 : bot::playIndexP2;
    double currentFrac = 0.0;

    while (idx < bot::currentMacro.actions.size()) {
        const auto& act = bot::currentMacro.actions[idx];
        if (act.p1 != isP1) {
            idx++;
            continue;
        }
        if (act.step > bot::currentStep) {
            break;
        }

        double targetFrac = (act.step == bot::currentStep) ? act.frac : currentFrac;
        if (targetFrac < currentFrac) targetFrac = currentFrac;

        double deltaFrac = targetFrac - currentFrac;
        if (deltaFrac > 1e-6) {
            player->update(static_cast<float>(deltaFrac * dt));
            currentFrac = targetFrac;
        }

        bot::isReplaying = true;
        pl->handleButton(act.down, act.btn, act.p1);
        bot::isReplaying = false;

        idx++;
    }

    double remainingFrac = 1.0 - currentFrac;
    if (remainingFrac > 1e-6) {
        player->update(static_cast<float>(remainingFrac * dt));
    }
}

class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool push, int button, bool player1) {
        if (bot::state == BotState::Play) {
            if (bot::isReplaying) {
                GJBaseGameLayer::handleButton(push, button, player1);
            }
            return;
        }

        if (bot::state == BotState::Record) {
            auto now = std::chrono::steady_clock::now();
            double elapsedSec = std::chrono::duration<double>(now - bot::stepStartTime).count();
            double speed = (bot::recSpeed > 0.0001) ? bot::recSpeed : 1.0;
            double stepDurationSec = (1.0 / 240.0) / speed;
            double stepsElapsed = elapsedSec / stepDurationSec;
            uint32_t stepOffset = static_cast<uint32_t>(stepsElapsed);
            double frac = stepsElapsed - static_cast<double>(stepOffset);
            if (frac < 0.0) frac = 0.0;
            if (frac > 0.999999) frac = 0.999999;
            uint32_t targetStep = bot::currentStep + stepOffset;

            bot::currentMacro.actions.push_back({
                targetStep,
                frac,
                push,
                player1,
                static_cast<uint8_t>(button)
            });

            GJBaseGameLayer::handleButton(push, button, player1);
            return;
        }

        GJBaseGameLayer::handleButton(push, button, player1);
    }
};

class $modify(BotPlayerObject, PlayerObject) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (!pl) {
            PlayerObject::update(dt);
            return;
        }

        bool isP1 = (this == pl->m_player1);
        bool isP2 = (this == pl->m_player2);

        if (!isP1 && !isP2) {
            PlayerObject::update(dt);
            return;
        }

        if (isP1) {
            bot::currentStep++;
            bot::stepStartTime = std::chrono::steady_clock::now();
        }

        if (bot::state == BotState::Play) {
            stepPlayerWithCBF(this, isP1, dt, pl);
        } else {
            PlayerObject::update(dt);
        }
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        this->m_clickBetweenSteps = false;
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        this->m_clickBetweenSteps = false;

        bot::currentStep = 0;
        bot::stepStartTime = std::chrono::steady_clock::now();
        bot::playIndexP1 = 0;
        bot::playIndexP2 = 0;

        if (!this->m_isPracticeMode) {
            bot::checkpoints.clear();
        }

        bot::isReplaying = true;
        this->handleButton(false, 1, true);
        this->handleButton(false, 2, true);
        this->handleButton(false, 3, true);
        this->handleButton(false, 1, false);
        this->handleButton(false, 2, false);
        this->handleButton(false, 3, false);
        bot::isReplaying = false;

        if (bot::state == BotState::Record) {
            cocos2d::CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
        } else {
            cocos2d::CCScheduler::get()->setTimeScale(1.0f);
        }
    }

    void onQuit() {
        cocos2d::CCScheduler::get()->setTimeScale(1.0f);
        bot::checkpoints.clear();
        PlayLayer::onQuit();
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (!cp) return cp;

        BotCheckpoint bcp;
        bcp.cp = cp;
        bcp.step = bot::currentStep;
        bcp.macroActionCount = bot::currentMacro.actions.size();
        bcp.playIndexP1 = bot::playIndexP1;
        bcp.playIndexP2 = bot::playIndexP2;

        if (this->m_player1) {
            bcp.posP1 = this->m_player1->getPosition();
            bcp.yVelP1 = this->m_player1->m_yAccel;
            bcp.rotP1 = this->m_player1->getRotation();
            bcp.isOnGroundP1 = this->m_player1->m_isOnGround;
            bcp.isUpsideDownP1 = this->m_player1->m_isUpsideDown;
        }
        if (this->m_player2) {
            bcp.posP2 = this->m_player2->getPosition();
            bcp.yVelP2 = this->m_player2->m_yAccel;
            bcp.rotP2 = this->m_player2->getRotation();
            bcp.isOnGroundP2 = this->m_player2->m_isOnGround;
            bcp.isUpsideDownP2 = this->m_player2->m_isUpsideDown;
        }

        bot::checkpoints.push_back(bcp);
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        if (!cp) return;

        for (auto it = bot::checkpoints.rbegin(); it != bot::checkpoints.rend(); ++it) {
            if (it->cp == cp) {
                bot::currentStep = it->step;

                if (bot::state == BotState::Record) {
                    if (bot::currentMacro.actions.size() > it->macroActionCount) {
                        bot::currentMacro.actions.resize(it->macroActionCount);
                    }
                } else if (bot::state == BotState::Play) {
                    bot::playIndexP1 = it->playIndexP1;
                    bot::playIndexP2 = it->playIndexP2;
                }

                if (this->m_player1) {
                    this->m_player1->setPosition(it->posP1);
                    this->m_player1->m_yAccel = it->yVelP1;
                    this->m_player1->setRotation(it->rotP1);
                    this->m_player1->m_isOnGround = it->isOnGroundP1;
                    this->m_player1->m_isUpsideDown = it->isUpsideDownP1;
                    if (this->m_player1->m_collisionLogTop) this->m_player1->m_collisionLogTop->removeAllObjects();
                    if (this->m_player1->m_collisionLogBottom) this->m_player1->m_collisionLogBottom->removeAllObjects();
                    if (this->m_player1->m_collisionLogLeft) this->m_player1->m_collisionLogLeft->removeAllObjects();
                    if (this->m_player1->m_collisionLogRight) this->m_player1->m_collisionLogRight->removeAllObjects();
                }
                if (this->m_player2) {
                    this->m_player2->setPosition(it->posP2);
                    this->m_player2->m_yAccel = it->yVelP2;
                    this->m_player2->setRotation(it->rotP2);
                    this->m_player2->m_isOnGround = it->isOnGroundP2;
                    this->m_player2->m_isUpsideDown = it->isUpsideDownP2;
                    if (this->m_player2->m_collisionLogTop) this->m_player2->m_collisionLogTop->removeAllObjects();
                    if (this->m_player2->m_collisionLogBottom) this->m_player2->m_collisionLogBottom->removeAllObjects();
                    if (this->m_player2->m_collisionLogLeft) this->m_player2->m_collisionLogLeft->removeAllObjects();
                    if (this->m_player2->m_collisionLogRight) this->m_player2->m_collisionLogRight->removeAllObjects();
                }

                bot::isReplaying = true;
                this->handleButton(false, 1, true);
                this->handleButton(false, 2, true);
                this->handleButton(false, 3, true);
                this->handleButton(false, 1, false);
                this->handleButton(false, 2, false);
                this->handleButton(false, 3, false);
                bot::isReplaying = false;

                break;
            }
        }
    }

    void removeLastCheckpoint() {
        PlayLayer::removeLastCheckpoint();
        if (!bot::checkpoints.empty()) {
            bot::checkpoints.pop_back();
        }
    }
};

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
            if (bot::state == BotState::Record) {
                st = fmt::format("REC ({} acts)", bot::currentMacro.actions.size());
            } else if (bot::state == BotState::Play) {
                st = fmt::format("PLAY ({} acts)", bot::currentMacro.actions.size());
            }
            m_fields->m_statusLabel->setString(st.c_str());
        }
        if (m_fields->m_offSprite) {
            m_fields->m_offSprite->setColor(bot::state == BotState::Off ? cocos2d::ccColor3B{ 80, 255, 80 } : cocos2d::ccColor3B{ 255, 255, 255 });
        }
        if (m_fields->m_recSprite) {
            m_fields->m_recSprite->setColor(bot::state == BotState::Record ? cocos2d::ccColor3B{ 255, 80, 80 } : cocos2d::ccColor3B{ 255, 255, 255 });
        }
        if (m_fields->m_playSprite) {
            m_fields->m_playSprite->setColor(bot::state == BotState::Play ? cocos2d::ccColor3B{ 80, 200, 255 } : cocos2d::ccColor3B{ 255, 255, 255 });
        }
    }

    void refreshMacroList() {
        if (!m_fields->m_fileListContainer) return;
        m_fields->m_fileListContainer->removeAllChildren();

        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(bot::getMacroDir(), ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".gdbot") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return a.filename().string() > b.filename().string();
        });

        float viewW = 160.0f;
        float viewH = 100.0f;
        float rowH = 22.0f;
        float totalH = std::max(viewH, static_cast<float>(files.size()) * rowH);

        auto scroll = ScrollLayer::create(cocos2d::CCSize{ viewW, viewH });
        scroll->m_contentLayer->setContentSize(cocos2d::CCSize{ viewW, totalH });

        for (size_t i = 0; i < files.size(); ++i) {
            auto path = files[i];
            float y = totalH - static_cast<float>(i + 1) * rowH;

            auto rowNode = cocos2d::CCNode::create();
            rowNode->setPosition(0.0f, y);
            rowNode->setContentSize(cocos2d::CCSize{ viewW, rowH });

            std::string name = path.stem().string();
            if (name.length() > 14) {
                name = name.substr(0, 12) + "..";
            }
            auto lbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
            lbl->setScale(0.4f);
            lbl->setAnchorPoint(cocos2d::CCPoint{ 0.0f, 0.5f });
            lbl->setPosition(4.0f, rowH / 2.0f);
            rowNode->addChild(lbl);

            auto rowMenu = cocos2d::CCMenu::create();
            rowMenu->setPosition(0.0f, 0.0f);

            auto loadSpr = ButtonSprite::create("L", 12, true, "goldFont.fnt", "GJ_button_01.png", 16.0f, 0.4f);
            auto loadBtn = CCMenuItemExt::createSpriteExtra(loadSpr, [this, path](CCObject*) {
                if (bot::loadMacro(path, bot::currentMacro)) {
                    bot::state = BotState::Play;
                    cocos2d::CCScheduler::get()->setTimeScale(1.0f);
                    this->updateStatusUI();
                    Notification::create("Macro Loaded", NotificationIcon::Success)->show();
                } else {
                    Notification::create("Failed to Load", NotificationIcon::Error)->show();
                }
            });
            loadBtn->setPosition(120.0f, rowH / 2.0f);
            rowMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("X", 12, true, "goldFont.fnt", "GJ_button_06.png", 16.0f, 0.4f);
            auto delBtn = CCMenuItemExt::createSpriteExtra(delSpr, [this, path](CCObject*) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
                this->refreshMacroList();
                Notification::create("Macro Deleted", NotificationIcon::Info)->show();
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

        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        float panelW = 170.0f;
        float panelH = 260.0f;
        float panelX = winSize.width - panelW - 8.0f;
        float panelY = (winSize.height - panelH) / 2.0f;

        auto panelBg = cocos2d::CCLayerColor::create(cocos2d::ccc4(0, 0, 0, 190), panelW, panelH);
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

        auto modeMenu = cocos2d::CCMenu::create();
        modeMenu->setPosition(0.0f, 0.0f);

        m_fields->m_offSprite = ButtonSprite::create("OFF", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto offBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_offSprite, [this](CCObject*) {
            bot::state = BotState::Off;
            cocos2d::CCScheduler::get()->setTimeScale(1.0f);
            this->updateStatusUI();
        });
        offBtn->setPosition(panelW * 0.2f, panelH - 58.0f);
        modeMenu->addChild(offBtn);

        m_fields->m_recSprite = ButtonSprite::create("REC", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto recBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_recSprite, [this](CCObject*) {
            bot::state = BotState::Record;
            bot::currentMacro.actions.clear();
            bot::currentStep = 0;
            bot::stepStartTime = std::chrono::steady_clock::now();
            cocos2d::CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
            this->updateStatusUI();
            Notification::create("Recording Started", NotificationIcon::Info)->show();
        });
        recBtn->setPosition(panelW * 0.5f, panelH - 58.0f);
        modeMenu->addChild(recBtn);

        m_fields->m_playSprite = ButtonSprite::create("PLAY", 24, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto playBtn = CCMenuItemExt::createSpriteExtra(m_fields->m_playSprite, [this](CCObject*) {
            bot::state = BotState::Play;
            bot::playIndexP1 = 0;
            bot::playIndexP2 = 0;
            cocos2d::CCScheduler::get()->setTimeScale(1.0f);
            this->updateStatusUI();
            Notification::create("Playback Ready", NotificationIcon::Info)->show();
        });
        playBtn->setPosition(panelW * 0.8f, panelH - 58.0f);
        modeMenu->addChild(playBtn);

        panelBg->addChild(modeMenu);

        auto speedLbl = CCLabelBMFont::create("Speed:", "chatFont.fnt");
        speedLbl->setScale(0.45f);
        speedLbl->setAnchorPoint(cocos2d::CCPoint{ 0.0f, 0.5f });
        speedLbl->setPosition(10.0f, panelH - 84.0f);
        panelBg->addChild(speedLbl);

        auto speedInput = TextInput::create(65.0f, "1.00", "chatFont.fnt");
        speedInput->setString(fmt::format("{:.2f}", bot::recSpeed));
        speedInput->setFilter("0123456789.,");
        speedInput->setScale(0.7f);
        speedInput->setPosition(100.0f, panelH - 84.0f);
        speedInput->setCallback([](std::string const& str) {
            bot::recSpeed = bot::parseSpeedString(str);
            if (bot::state == BotState::Record) {
                cocos2d::CCScheduler::get()->setTimeScale(static_cast<float>(bot::recSpeed));
            }
        });
        panelBg->addChild(speedInput);

        auto saveMenu = cocos2d::CCMenu::create();
        saveMenu->setPosition(0.0f, 0.0f);
        auto saveSpr = ButtonSprite::create("SAVE CURRENT", 80, true, "goldFont.fnt", "GJ_button_01.png", 20.0f, 0.4f);
        auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](CCObject*) {
            if (bot::currentMacro.actions.empty()) {
                Notification::create("Macro is Empty", NotificationIcon::Warning)->show();
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
                tmStruct.tm_year + 1900,
                tmStruct.tm_mon + 1,
                tmStruct.tm_mday,
                tmStruct.tm_hour,
                tmStruct.tm_min,
                tmStruct.tm_sec
            );
            auto fullPath = bot::getMacroDir() / filename;
            if (bot::saveMacro(fullPath, bot::currentMacro)) {
                this->refreshMacroList();
                Notification::create("Saved Macro", NotificationIcon::Success)->show();
            } else {
                Notification::create("Save Failed", NotificationIcon::Error)->show();
            }
        });
        saveBtn->setPosition(panelW / 2.0f, panelH - 110.0f);
        saveMenu->addChild(saveBtn);
        panelBg->addChild(saveMenu);

        m_fields->m_fileListContainer = cocos2d::CCNode::create();
        m_fields->m_fileListContainer->setPosition(5.0f, 15.0f);
        m_fields->m_fileListContainer->setContentSize(cocos2d::CCSize{ 160.0f, 100.0f });
        panelBg->addChild(m_fields->m_fileListContainer);

        this->updateStatusUI();
        this->refreshMacroList();
    }
};
