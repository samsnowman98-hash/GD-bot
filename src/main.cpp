#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <fstream>
#include <chrono>
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

using Clock = std::chrono::steady_clock;
static double g_levelTimeAtUpdateEnd = 0.0;
static Clock::time_point g_clockAtUpdateEnd = Clock::now();

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

    // Выход из записи: сохраняем
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
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        g_levelTimeAtUpdateEnd = m_gameState.m_levelTime;
        g_clockAtUpdateEnd = Clock::now();
    }

    // Сырое время клика: время уровня + реальное время после последнего кадра
    double rawTime() {
        double elapsed = std::chrono::duration<double>(Clock::now() - g_clockAtUpdateEnd).count();
        elapsed = std::clamp(elapsed, 0.0, 1.0 / 30.0);
        return g_levelTimeAtUpdateEnd + elapsed;
    }

    void pushButton(int button, bool isPlayer1) {
        if (!g_injecting && PlayLayer::get()) {
            if (g_mode == BotMode::Play) return;
            if (g_mode == BotMode::Record) {
                g_macro.push_back({this->rawTime(), true, button, isPlayer1});
            }
        }
        GJBaseGameLayer::pushButton(button, isPlayer1);
    }

    void releaseButton(int button, bool isPlayer1) {
        if (!g_injecting && PlayLayer::get()) {
            if (g_mode == BotMode::Play) return;
            if (g_mode == BotMode::Record) {
                g_macro.push_back({this->rawTime(), false, button, isPlayer1});
            }
        }
        GJBaseGameLayer::releaseButton(button, isPlayer1);
    }

    void processCommands(float dt) {
        if (g_mode == BotMode::Play && PlayLayer::get()) {
            double t = m_gameState.m_levelTime;
            while (g_index < g_macro.size() && g_macro[g_index].time <= t + dt * 0.5) {
                auto& c = g_macro[g_index++];
                g_injecting = true;
                if (c.down) GJBaseGameLayer::pushButton(c.button, c.player1);
                else        GJBaseGameLayer::releaseButton(c.button, c.player1);
                g_injecting = false;
            }
        }
        GJBaseGameLayer::processCommands(dt);
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

// Три кнопки в меню паузы: Запись / Off / Воспроизведение
class $modify(RawBotPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu, 10);

        struct Entry { const char* text; BotMode mode; const char* sprite; };
        Entry entries[3] = {
            {"Rec",  BotMode::Record, "GJ_button_06.png"}, // красноватая
            {"Off",  BotMode::Off,    "GJ_button_04.png"}, // серая
            {"Play", BotMode::Play,   "GJ_button_01.png"}  // зелёная
        };

        float startX = win.width - 150.f;
        float y = 40.f;

        for (int i = 0; i < 3; i++) {
            auto mode = entries[i].mode;
            auto spr = ButtonSprite::create(entries[i].text, 60, true, "goldFont.fnt",
                                            entries[i].sprite, 35.f, 0.7f);
            // Подсветка активного режима
            spr->setOpacity(g_mode == mode ? 255 : 130);

            auto btn = CCMenuItemExt::createSpriteExtra(spr, [mode](CCObject*) {
                setMode(mode, PlayLayer::get());
            });
            btn->setTag(static_cast<int>(mode));
            btn->setPosition({startX + i * 70.f, y});
            menu->addChild(btn);
        }

        // Обновление подсветки после нажатия
        menu->schedule(schedule_selector(CCMenu::update), 0.1f);
    }
};
