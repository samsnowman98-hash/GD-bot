#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <fstream>
#include <algorithm>

using namespace geode::prelude;

enum class BotMode { Off, Record, Play };

struct RawClick {
    double time;   // секунды от начала уровня, не зависит от TPS
    bool down;
    int button;
    bool player1;
};

static BotMode g_mode = BotMode::Off;
static std::vector<RawClick> g_macro;
static size_t g_index = 0;
static bool g_injecting = false;

static CCLabelBMFont* g_statusLabel = nullptr;

static const char* modeName(BotMode m) {
    switch (m) {
        case BotMode::Record: return "REC";
        case BotMode::Play:   return "PLAY";
        default:              return "OFF";
    }
}

static std::filesystem::path macroPath(PlayLayer* pl) {
    int id = pl && pl->m_level ? pl->m_level->m_levelID.value() : 0;
    return Mod::get()->getSaveDir() / fmt::format("level_{}.txt", id);
}

static void saveMacro(PlayLayer* pl) {
    if (g_macro.empty()) return;
    std::ofstream out(macroPath(pl));
    out.precision(9);
    for (auto& c : g_macro) {
        out << c.time << ' ' << c.down << ' ' << c.button << ' ' << c.player1 << '\n';
    }
    log::info("Макрос сохранён: {} кликов", g_macro.size());
}

static void loadMacro(PlayLayer* pl) {
    g_macro.clear();
    std::ifstream in(macroPath(pl));
    RawClick c{};
    int down, p1;
    while (in >> c.time >> down >> c.button >> p1) {
        c.down = down;
        c.player1 = p1;
        g_macro.push_back(c);
    }
    log::info("Макрос загружен: {} кликов", g_macro.size());
}

static void updateStatus() {
    if (!g_statusLabel) return;
    g_statusLabel->setString(modeName(g_mode));
    g_statusLabel->setVisible(g_mode != BotMode::Off);
    g_statusLabel->setColor(g_mode == BotMode::Record ? ccColor3B{255, 70, 70} : ccColor3B{70, 255, 120});
}

static void setMode(BotMode newMode, PlayLayer* pl) {
    if (newMode == g_mode) return;

    if (g_mode == BotMode::Record) saveMacro(pl);

    g_mode = newMode;
    g_index = 0;

    if (g_mode == BotMode::Record) {
        g_macro.clear();
    } else if (g_mode == BotMode::Play) {
        loadMacro(pl);
    }
    updateStatus();
}

class $modify(RawBotBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_injecting && PlayLayer::get()) {
            if (g_mode == BotMode::Play) return; // блок ручного ввода при воспроизведении
            if (g_mode == BotMode::Record) {
                g_macro.push_back({m_gameState.m_levelTime, down, button, isPlayer1});
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_mode == BotMode::Play && PlayLayer::get()) {
            double t = m_gameState.m_levelTime;
            while (g_index < g_macro.size() && g_macro[g_index].time <= t + dt * 0.5) {
                auto& c = g_macro[g_index++];
                g_injecting = true;
                GJBaseGameLayer::handleButton(c.down, c.button, c.player1);
                g_injecting = false;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(RawBotPlay, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        g_statusLabel = CCLabelBMFont::create("OFF", "bigFont.fnt");
        g_statusLabel->setScale(0.45f);
        g_statusLabel->setOpacity(160);
        g_statusLabel->setAnchorPoint({0.f, 1.f});
        auto win = CCDirector::get()->getWinSize();
        g_statusLabel->setPosition({8.f, win.height - 6.f});
        this->getParent()->addChild(g_statusLabel, 100);
        updateStatus();

        if (g_mode == BotMode::Play) loadMacro(this);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        double t = m_gameState.m_levelTime;

        if (g_mode == BotMode::Record) {
            g_macro.erase(
                std::remove_if(g_macro.begin(), g_macro.end(),
                    [t](const RawClick& c) { return c.time >= t; }),
                g_macro.end());
        }
        if (g_mode == BotMode::Play) {
            g_index = std::lower_bound(g_macro.begin(), g_macro.end(), t,
                [](const RawClick& c, double v) { return c.time < v; }) - g_macro.begin();
        }
    }

    void levelComplete() {
        if (g_mode == BotMode::Record) saveMacro(this);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (g_mode == BotMode::Record) saveMacro(this);
        g_statusLabel = nullptr;
        PlayLayer::onQuit();
    }
};

// Три кнопки в меню паузы: Rec / Off / Play
class $modify(RawBotPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu, 10);

        struct Entry { const char* text; BotMode mode; const char* sprite; };
        Entry entries[3] = {
            {"Rec",  BotMode::Record, "GJ_button_06.png"},
            {"Off",  BotMode::Off,    "GJ_button_04.png"},
            {"Play", BotMode::Play,   "GJ_button_01.png"}
        };

        auto buttons = std::make_shared<std::vector<ButtonSprite*>>();
        float startX = win.width - 150.f;
        float y = 40.f;

        for (int i = 0; i < 3; i++) {
            auto mode = entries[i].mode;
            auto spr = ButtonSprite::create(entries[i].text, 60, true, "goldFont.fnt",
                                            entries[i].sprite, 35.f, 0.7f);
            spr->setOpacity(g_mode == mode ? 255 : 130);
            buttons->push_back(spr);

            auto btn = CCMenuItemExt::createSpriteExtra(spr, [mode, buttons](CCObject*) {
                setMode(mode, PlayLayer::get());
                // живая подсветка активного режима
                static const BotMode order[3] = {BotMode::Record, BotMode::Off, BotMode::Play};
                for (int k = 0; k < 3; k++) {
                    (*buttons)[k]->setOpacity(g_mode == order[k] ? 255 : 130);
                }
            });
            btn->setPosition({startX + i * 70.f, y});
            menu->addChild(btn);
        }
    }
};
