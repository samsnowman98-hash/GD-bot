#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/binding/CheckpointObject.hpp>

using namespace geode::prelude;

enum class BotMode {
    Disabled,
    Record,
    Play
};

struct BotAction {
    double time;
    bool push;
    int button;
    bool player1;
};

struct CheckpointSnapshot {
    double time;
    size_t actionCount;
};

// Переменные состояния бота
static BotMode s_mode = BotMode::Disabled;
static double s_time = 0.0;
static size_t s_actionIndex = 0;
static bool s_isReplaying = false;

static std::vector<BotAction> s_actions;
static std::vector<CheckpointSnapshot> s_checkpoints;

// 1. ХУК НА ОБНОВЛЕНИЕ КАДРОВ И ФИЗИКИ
class $modify(BotPlayLayer, PlayLayer) {
    void update(float dt) {
        if (s_mode != BotMode::Disabled) {
            s_time += static_cast<double>(dt);

            // Если включен бот — отдаем клики перед расчетом кадра
            if (s_mode == BotMode::Play) {
                while (s_actionIndex < s_actions.size() && s_time >= s_actions[s_actionIndex].time) {
                    auto const& act = s_actions[s_actionIndex];
                    s_isReplaying = true;
                    this->handleButton(act.push, act.button, act.player1);
                    s_isReplaying = false;
                    s_actionIndex++;
                }
            }
        }

        PlayLayer::update(dt);
    }

    // Сохранение чекпоинта практики
    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (s_mode != BotMode::Disabled) {
            s_checkpoints.push_back({ s_time, s_actions.size() });
        }
        return cp;
    }

    // Удаление чекпоинта практики (исправлено имя функции!)
    void removeCheckpoint(bool p0) {
        PlayLayer::removeCheckpoint(p0);
        if (!s_checkpoints.empty()) {
            s_checkpoints.pop_back();
        }
    }

    // Респавн на чекпоинте при смерти
    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        if (!s_checkpoints.empty()) {
            auto snap = s_checkpoints.back();
            s_time = snap.time;

            if (s_mode == BotMode::Record) {
                if (snap.actionCount <= s_actions.size()) {
                    s_actions.resize(snap.actionCount); // Удаляем неудачные клики после смерти
                }
            } else if (s_mode == BotMode::Play) {
                s_actionIndex = snap.actionCount; // Перематываем бота к чекпоинту
            }
        }
        // Отжимаем кнопки, чтобы куб не зажимал прыжок бесконечно
        this->handleButton(false, 1, true);
        this->handleButton(false, 1, false);
    }

    // Рестарт уровня
    void resetLevel() {
        PlayLayer::resetLevel();

        if (!m_isPracticeMode || s_checkpoints.empty()) {
            s_time = 0.0;
            s_actionIndex = 0;
            s_checkpoints.clear();

            if (s_mode == BotMode::Record) {
                s_actions.clear();
            }
        }
    }

    // Выход из уровня
    void onQuit() {
        PlayLayer::onQuit();
        s_checkpoints.clear();
        s_time = 0.0;
        s_actionIndex = 0;
    }
};

// 2. ХУК НА НАЖАТИЯ КНОПОК
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool push, int button, bool player1) {
        // Пропускаем клики самого бота без повторной записи
        if (s_isReplaying) {
            GJBaseGameLayer::handleButton(push, button, player1);
            return;
        }

        // Если бот играет сам — игнорируем пальцы игрока
        if (s_mode == BotMode::Play) {
            return;
        }

        // Если идет запись — запоминаем время и тип нажатия
        if (s_mode == BotMode::Record) {
            s_actions.push_back({ s_time, push, button, player1 });
        }

        GJBaseGameLayer::handleButton(push, button, player1);
    }
};

// 3. УПРАВЛЕНИЕ В МЕНЮ ПАУЗЫ
class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        auto menu = CCMenu::create();
        menu->setPosition({ winSize.width / 2.f, 35.f });
        menu->setID("bot-control-menu"_spr);

        const char* modeTitle = "Bot: OFF";
        if (s_mode == BotMode::Record) modeTitle = "Bot: REC";
        else if (s_mode == BotMode::Play) modeTitle = "Bot: PLAY";

        auto modeSpr = ButtonSprite::create(modeTitle);
        modeSpr->setScale(0.7f);
        auto modeBtn = CCMenuItemSpriteExtra::create(
            modeSpr, this, menu_selector(BotPauseLayer::onToggleMode)
        );

        auto clearSpr = ButtonSprite::create("Clear");
        clearSpr->setScale(0.7f);
        auto clearBtn = CCMenuItemSpriteExtra::create(
            clearSpr, this, menu_selector(BotPauseLayer::onClearMacro)
        );

        menu->addChild(modeBtn);
        menu->addChild(clearBtn);

        auto layout = RowLayout::create();
        layout->setGap(12.f);
        menu->setLayout(layout);
        menu->updateLayout();

        this->addChild(menu);
    }

    void onToggleMode(CCObject* sender) {
        const char* modeTitle = "Bot: OFF";

        if (s_mode == BotMode::Disabled) {
            s_mode = BotMode::Record;
            modeTitle = "Bot: REC";
            Notification::create("Bot: RECORDING", NotificationIcon::Info)->show();
        } else if (s_mode == BotMode::Record) {
            s_mode = BotMode::Play;
            modeTitle = "Bot: PLAY";
            Notification::create("Bot: PLAYING", NotificationIcon::Success)->show();
        } else {
            s_mode = BotMode::Disabled;
            Notification::create("Bot: OFF", NotificationIcon::Warning)->show();
        }

        if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(sender)) {
            auto spr = ButtonSprite::create(modeTitle);
            spr->setScale(0.7f);
            btn->setNormalImage(spr);
        }
    }

    void onClearMacro(CCObject*) {
        s_actions.clear();
        s_checkpoints.clear();
        s_time = 0.0;
        s_actionIndex = 0;
        Notification::create("Macro Cleared!", NotificationIcon::Success)->show();
    }
};
