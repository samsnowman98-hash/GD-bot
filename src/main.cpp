// Simple frame-perfect macro bot for Geometry Dash 2.2081 / Geode 5.x (Android)
//
// Идея:
//  * Время = номер физического шага (processCommands), а не секунды -> не зависит от FPS.
//  * Запись: хук handleButton пишет {frame, button, down, p1}.
//  * Воспроизведение: в начале processCommands подаём действия этого кадра, реальный ввод блокируется.
//  * Практик-режим: каждому чекпоинту запоминаем кадр; при откате обрезаем макрос / перематываем индекс.
//  * Макрос сохраняется в файл (отдельный на каждый уровень) и грузится при входе в уровень.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace bot {

    enum class Mode { Off, Record, Play };

    struct Action {
        uint32_t frame;
        uint8_t button;
        bool down;
        bool p1;
    };

    struct State {
        Mode mode = Mode::Off;
        uint32_t frame = 0;          // номер следующего физического шага
        size_t index = 0;            // следующее действие для воспроизведения
        int inject = 0;              // >0 пока мы сами вызываем handleButton
        bool cpLoaded = false;       // resetLevel загрузил чекпоинт
        int levelID = 0;
        std::vector<Action> macro;
        std::unordered_map<CheckpointObject*, uint32_t> checkpoints;
    };

    inline State g;

    inline bool isPlayLayer(GJBaseGameLayer* l) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == l;
    }

    inline char const* modeName() {
        switch (g.mode) {
            case Mode::Record: return "REC";
            case Mode::Play: return "PLAY";
            default: return "OFF";
        }
    }

    inline std::string statusText() {
        return fmt::format("Bot: {} | actions: {}", modeName(), g.macro.size());
    }

    inline std::filesystem::path macroPath() {
        return Mod::get()->getSaveDir() / fmt::format("level_{}.macro", g.levelID);
    }

    constexpr uint32_t kMagic = 0x42544F31; // "1OTB"

    inline void saveMacro() {
        std::ofstream f(macroPath(), std::ios::binary | std::ios::trunc);
        if (!f) {
            log::error("Cannot open macro file for writing");
            return;
        }
        uint32_t magic = kMagic;
        uint32_t count = static_cast<uint32_t>(g.macro.size());
        f.write(reinterpret_cast<char const*>(&magic), sizeof(magic));
        f.write(reinterpret_cast<char const*>(&count), sizeof(count));
        for (auto const& a : g.macro) {
            uint8_t flags = static_cast<uint8_t>((a.down ? 1 : 0) | (a.p1 ? 2 : 0));
            f.write(reinterpret_cast<char const*>(&a.frame), sizeof(a.frame));
            f.write(reinterpret_cast<char const*>(&flags), sizeof(flags));
            f.write(reinterpret_cast<char const*>(&a.button), sizeof(a.button));
        }
    }

    inline void loadMacro() {
        g.macro.clear();
        std::ifstream f(macroPath(), std::ios::binary);
        if (!f) return;
        uint32_t magic = 0, count = 0;
        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        f.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!f || magic != kMagic || count > 5'000'000) return;
        std::vector<Action> tmp;
        tmp.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Action a{};
            uint8_t flags = 0;
            f.read(reinterpret_cast<char*>(&a.frame), sizeof(a.frame));
            f.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            f.read(reinterpret_cast<char*>(&a.button), sizeof(a.button));
            if (!f) return; // битый файл - не грузим
            a.down = flags & 1;
            a.p1 = flags & 2;
            tmp.push_back(a);
        }
        g.macro = std::move(tmp);
    }

    inline void injectButton(GJBaseGameLayer* l, bool down, int button, bool p1) {
        ++g.inject;
        l->handleButton(down, button, p1);
        --g.inject;
    }

    // Перематывает воспроизведение на g.frame и восстанавливает зажатые кнопки
    inline void syncPlayback(GJBaseGameLayer* l) {
        auto it = std::lower_bound(
            g.macro.begin(), g.macro.end(), g.frame,
            [](Action const& a, uint32_t f) { return a.frame < f; }
        );
        g.index = static_cast<size_t>(it - g.macro.begin());

        bool held[4][2] = {};
        for (size_t i = 0; i < g.index; ++i) {
            auto const& a = g.macro[i];
            if (a.button < 4) held[a.button][a.p1 ? 0 : 1] = a.down;
        }
        for (int b = 1; b <= 3; ++b) {
            injectButton(l, held[b][0], b, true);
            injectButton(l, held[b][1], b, false);
        }
    }

    inline void notify(char const* text, NotificationIcon icon = NotificationIcon::Info) {
        Notification::create(text, icon, 1.f)->show();
    }
}

// ---------------------------------------------------------------------------
// Ввод и счётчик кадров
// ---------------------------------------------------------------------------
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (bot::g.inject > 0 || bot::g.mode == bot::Mode::Off || !bot::isPlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        if (bot::g.mode == bot::Mode::Play) return; // реальный ввод во время воспроизведения игнорируем

        // Record
        bot::g.macro.push_back({
            bot::g.frame, static_cast<uint8_t>(button), down, isPlayer1
        });
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // В GD 2.2081 сигнатура: processCommands(float dt, bool isHalfTick, bool isLastTick)
    // Считаем КАЖДЫЙ вызов: запись и воспроизведение используют один и тот же счётчик,
    // поэтому они всегда совпадают друг с другом.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!bot::isPlayLayer(this)) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        if (bot::g.mode == bot::Mode::Play) {
            auto& g = bot::g;
            while (g.index < g.macro.size() && g.macro[g.index].frame <= g.frame) {
                auto const& a = g.macro[g.index];
                bot::injectButton(this, a.down, a.button, a.p1);
                ++g.index;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        ++bot::g.frame;
    }
};

// ---------------------------------------------------------------------------
// Уровень, чекпоинты, рестарты
// ---------------------------------------------------------------------------
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bot::g = bot::State{};
        if (level) bot::g.levelID = level->m_levelID.value();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        bot::loadMacro();
        return true;
    }

    void resetLevel() {
        bot::g.cpLoaded = false;
        PlayLayer::resetLevel();
        if (bot::g.cpLoaded) return; // откат на чекпоинт обработан в loadFromCheckpoint

        bot::g.frame = 0;
        bot::g.index = 0;
        bot::g.checkpoints.clear();
        if (bot::g.mode == bot::Mode::Record) bot::g.macro.clear();
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (cp) bot::g.checkpoints[cp] = bot::g.frame;
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        bot::g.cpLoaded = true;

        auto it = bot::g.checkpoints.find(cp);
        if (it == bot::g.checkpoints.end()) {
            // неизвестный чекпоинт - синхронизироваться нельзя, безопаснее выключить бота
            bot::g.frame = 0;
            bot::g.index = 0;
            if (bot::g.mode != bot::Mode::Off) {
                bot::g.mode = bot::Mode::Off;
                bot::notify("Bot: unknown checkpoint, OFF", NotificationIcon::Warning);
            }
            return;
        }

        bot::g.frame = it->second;

        if (bot::g.mode == bot::Mode::Record) {
            auto& m = bot::g.macro;
            m.erase(
                std::remove_if(m.begin(), m.end(), [](bot::Action const& a) {
                    return a.frame >= bot::g.frame;
                }),
                m.end()
            );
        }
        else if (bot::g.mode == bot::Mode::Play) {
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
        }
        PlayLayer::levelComplete();
    }
};

// ---------------------------------------------------------------------------
// Кнопки в меню паузы
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
        label->setScale(0.6f);
        label->setAnchorPoint({ 0.f, 0.5f });
        label->setPosition({ 10.f, win.height / 2.f + 75.f });
        this->addChild(label);

        float x = 45.f;
        float y = win.height / 2.f + 45.f;

        auto add = [&](char const* text, char const* id, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(text);
            spr->setScale(0.6f);
            auto item = CCMenuItemSpriteExtra::create(spr, this, cb);
            item->setID(id);
            item->setPosition({ x, y });
            menu->addChild(item);
            y -= 30.f;
        };

        add("REC",   "bot-rec",   menu_selector(BotPauseLayer::onBotRec));
        add("PLAY",  "bot-play",  menu_selector(BotPauseLayer::onBotPlay));
        add("STOP",  "bot-stop",  menu_selector(BotPauseLayer::onBotStop));
        add("CLEAR", "bot-clear", menu_selector(BotPauseLayer::onBotClear));

        this->addChild(menu);
    }

    void updateBotLabel() {
        if (auto l = typeinfo_cast<CCLabelBMFont*>(this->getChildByID("bot-status"))) {
            l->setString(bot::statusText().c_str());
        }
    }

    void onBotRec(CCObject*) {
        bot::g.macro.clear();
        bot::g.mode = bot::Mode::Record;
        bot::notify("Recording...", NotificationIcon::Info);
        this->onRestart(nullptr); // рестарт с начала и выход из паузы
    }

    void onBotPlay(CCObject*) {
        if (bot::g.macro.empty()) {
            bot::notify("No macro!", NotificationIcon::Warning);
            return;
        }
        bot::g.mode = bot::Mode::Play;
        bot::notify("Playing...", NotificationIcon::Success);
        this->onRestart(nullptr);
    }

    void onBotStop(CCObject*) {
        if (bot::g.mode == bot::Mode::Record && !bot::g.macro.empty()) {
            bot::saveMacro();
            bot::notify("Macro saved!", NotificationIcon::Success);
        }
        bot::g.mode = bot::Mode::Off;
        this->updateBotLabel();
    }

    void onBotClear(CCObject*) {
        bot::g.macro.clear();
        bot::g.mode = bot::Mode::Off;
        std::error_code ec;
        std::filesystem::remove(bot::macroPath(), ec);
        bot::notify("Macro cleared", NotificationIcon::Success);
        this->updateBotLabel();
    }
};
