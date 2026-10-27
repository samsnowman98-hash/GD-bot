// GD Bot — macro recorder / replayer for Geometry Dash 2.2081 (Geode)
//
// Модель работы:
//  * Макрос = список событий {frame, button, down, player1}, где frame — номер физического шага (240 TPS).
//  * Запись: хук GJBaseGameLayer::handleButton пишет все нажатия/отпускания с текущим номером шага.
//  * Воспроизведение: перед каждым шагом (processCommands) инжектим события этого шага,
//    а реальный ввод блокируем.
//  * Практика: при загрузке чекпоинта счётчик шагов откатывается на шаг, на котором чекпоинт был создан,
//    а запись обрезается до этого шага — дальше она перезаписывается.

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

    enum class Mode { Off = 0, Record = 1, Play = 2 };

    struct Input {
        uint32_t frame;
        uint8_t button;
        bool down;
        bool p1;
    };

    struct State {
        Mode mode = Mode::Off;
        uint32_t frame = 0;                 // номер текущего физического шага
        std::vector<Input> macro;           // всегда отсортирован по frame
        size_t playIdx = 0;                 // следующее событие для воспроизведения
        bool syncHeld = false;              // после перемотки нужно восстановить зажатые кнопки
        bool injecting = false;             // сейчас вводим события сами
        bool loadedFromCheckpoint = false;  // resetLevel загрузил чекпоинт
        std::unordered_map<CheckpointObject*, uint32_t> cpFrames;
        std::string levelKey;
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool isPlayLayer(GJBaseGameLayer* gl) {
        return gl != nullptr && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == gl;
    }

    // ---------- работа с макросом ----------

    // удалить все события с frame >= f
    inline void truncateFrom(uint32_t f) {
        auto& s = state();
        auto it = std::lower_bound(
            s.macro.begin(), s.macro.end(), f,
            [](Input const& i, uint32_t v) { return i.frame < v; }
        );
        s.macro.erase(it, s.macro.end());
    }

    // после обрезки: если макрос оставил какую-то кнопку зажатой, а игра после
    // загрузки чекпоинта её отпустила — добавляем явное отпускание на шаге f
    inline void releaseHeld(uint32_t f) {
        auto& s = state();
        bool held[2][4] = {};
        for (auto const& e : s.macro) {
            if (e.button < 4) held[e.p1 ? 0 : 1][e.button] = e.down;
        }
        for (int p = 0; p < 2; ++p) {
            for (uint8_t b = 0; b < 4; ++b) {
                if (held[p][b]) s.macro.push_back({f, b, false, p == 0});
            }
        }
    }

    // перемотка счётчика шагов (и указателя воспроизведения) на шаг f
    inline void seek(uint32_t f) {
        auto& s = state();
        s.frame = f;
        s.playIdx = std::lower_bound(
            s.macro.begin(), s.macro.end(), f,
            [](Input const& i, uint32_t v) { return i.frame < v; }
        ) - s.macro.begin();
        s.syncHeld = true;
    }

    // сброс уровня на самое начало
    inline void resetToStart() {
        auto& s = state();
        seek(0);
        if (s.mode == Mode::Record) truncateFrom(0);
    }

    // инжект событий текущего шага
    inline void inject(GJBaseGameLayer* gl) {
        auto& s = state();
        s.injecting = true;

        if (s.syncHeld) {
            s.syncHeld = false;
            bool held[2][4] = {};
            for (size_t i = 0; i < s.playIdx && i < s.macro.size(); ++i) {
                auto const& e = s.macro[i];
                if (e.button < 4) held[e.p1 ? 0 : 1][e.button] = e.down;
            }
            for (int p = 0; p < 2; ++p) {
                for (int b = 0; b < 4; ++b) {
                    if (held[p][b]) gl->handleButton(true, b, p == 0);
                }
            }
        }

        while (s.playIdx < s.macro.size() && s.macro[s.playIdx].frame <= s.frame) {
            auto const e = s.macro[s.playIdx++];
            gl->handleButton(e.down, e.button, e.p1);
        }

        s.injecting = false;
    }

    // ---------- сохранение / загрузка ----------

    inline std::filesystem::path macroPath() {
        return Mod::get()->getSaveDir() / "macros" / (state().levelKey + ".gdbm");
    }

    inline void save() {
        auto& s = state();
        if (s.levelKey.empty()) return;

        std::error_code ec;
        std::filesystem::create_directories(Mod::get()->getSaveDir() / "macros", ec);

        std::ofstream f(macroPath(), std::ios::binary | std::ios::trunc);
        if (!f) {
            log::error("GDBot: can't open macro file for writing");
            return;
        }

        uint32_t version = 1;
        uint32_t count = static_cast<uint32_t>(s.macro.size());
        f.write("GDBM", 4);
        f.write(reinterpret_cast<char const*>(&version), 4);
        f.write(reinterpret_cast<char const*>(&count), 4);
        for (auto const& e : s.macro) {
            uint8_t flags = (e.down ? 1 : 0) | (e.p1 ? 2 : 0);
            f.write(reinterpret_cast<char const*>(&e.frame), 4);
            f.write(reinterpret_cast<char const*>(&e.button), 1);
            f.write(reinterpret_cast<char const*>(&flags), 1);
        }
        log::info("GDBot: saved {} inputs", count);
    }

    inline void load() {
        auto& s = state();
        s.macro.clear();
        if (s.levelKey.empty()) return;

        std::error_code ec;
        auto path = macroPath();
        if (!std::filesystem::exists(path, ec)) return;
        auto size = std::filesystem::file_size(path, ec);
        if (ec || size < 12) return;

        std::ifstream f(path, std::ios::binary);
        if (!f) return;

        char magic[4];
        uint32_t version = 0, count = 0;
        f.read(magic, 4);
        f.read(reinterpret_cast<char*>(&version), 4);
        f.read(reinterpret_cast<char*>(&count), 4);
        if (!f || std::string(magic, 4) != "GDBM" || version != 1) return;
        if (static_cast<uint64_t>(count) * 6 + 12 != size) return;

        s.macro.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Input e{};
            uint8_t flags = 0;
            f.read(reinterpret_cast<char*>(&e.frame), 4);
            f.read(reinterpret_cast<char*>(&e.button), 1);
            f.read(reinterpret_cast<char*>(&flags), 1);
            if (!f) { s.macro.clear(); return; }
            e.down = flags & 1;
            e.p1 = flags & 2;
            s.macro.push_back(e);
        }
        log::info("GDBot: loaded {} inputs", count);
    }

    inline void onEnterLevel(GJGameLevel* level) {
        auto& s = state();
        s.mode = Mode::Off;
        s.frame = 0;
        s.playIdx = 0;
        s.syncHeld = false;
        s.injecting = false;
        s.cpFrames.clear();

        int id = level->m_levelID.value();
        if (id > 0) {
            s.levelKey = std::to_string(id);
        } else {
            std::string name = level->m_levelName;
            for (auto& c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
            }
            s.levelKey = "local_" + name;
        }
        load();
    }

    inline char const* modeName(Mode m) {
        switch (m) {
            case Mode::Record: return "Recording";
            case Mode::Play: return "Playing";
            default: return "Off";
        }
    }
}

// =====================================================================
//  Физический шаг и ввод
// =====================================================================

class $modify(BotBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = bot::state();
        bool inPlay = bot::isPlayLayer(this);

        if (inPlay && s.mode == bot::Mode::Play) bot::inject(this);

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (inPlay) s.frame++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = bot::state();
        if (bot::isPlayLayer(this) && !s.injecting) {
            // при воспроизведении реальный ввод игнорируем
            if (s.mode == bot::Mode::Play) return;
            if (s.mode == bot::Mode::Record) {
                s.macro.push_back({s.frame, static_cast<uint8_t>(button), down, isPlayer1});
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// =====================================================================
//  Уровень: сбросы, чекпоинты
// =====================================================================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        bot::onEnterLevel(level);
        return true;
    }

    void resetLevel() {
        auto& s = bot::state();
        s.loadedFromCheckpoint = false;
        PlayLayer::resetLevel();
        // если чекпоинт не загружался — это рестарт с начала
        if (!s.loadedFromCheckpoint) bot::resetToStart();
    }

    void resetLevelFromStart() {
        PlayLayer::resetLevelFromStart();
        bot::state().cpFrames.clear();
        bot::resetToStart();
    }

    void storeCheckpoint(CheckpointObject* cp) {
        PlayLayer::storeCheckpoint(cp);
        bot::state().cpFrames[cp] = bot::state().frame;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);

        auto& s = bot::state();
        s.loadedFromCheckpoint = true;

        auto it = s.cpFrames.find(cp);
        if (it == s.cpFrames.end()) {
            log::warn("GDBot: unknown checkpoint, restarting macro from the beginning");
            bot::resetToStart();
            return;
        }

        uint32_t f = it->second;
        bot::seek(f);
        if (s.mode == bot::Mode::Record) {
            bot::truncateFrom(f);   // всё после чекпоинта перезаписывается
            bot::releaseHeld(f);
        }
    }

    void levelComplete() {
        if (bot::state().mode == bot::Mode::Record) bot::save();
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (bot::state().mode == bot::Mode::Record) bot::save();
        bot::state().mode = bot::Mode::Off;
        PlayLayer::onQuit();
    }
};

// =====================================================================
//  Меню паузы: Record / Off / Play
// =====================================================================

class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCMenu* menu = nullptr;
        CCLabelBMFont* label = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();

        m_fields->label = CCLabelBMFont::create("", "bigFont.fnt");
        m_fields->label->setScale(.45f);
        m_fields->label->setPosition({win.width / 2, 68.f});
        this->addChild(m_fields->label);

        auto menu = CCMenu::create();
        menu->setPosition({win.width / 2, 38.f});
        menu->setContentSize({300.f, 40.f});
        menu->setLayout(RowLayout::create()->setGap(10.f)->setAutoScale(false));
        this->addChild(menu);
        m_fields->menu = menu;

        this->rebuildBotMenu();
    }

    CCMenuItemSpriteExtra* makeBotButton(char const* text, bot::Mode mode) {
        bool selected = bot::state().mode == mode;
        auto spr = ButtonSprite::create(
            text, "bigFont.fnt",
            selected ? "GJ_button_01.png" : "GJ_button_04.png",
            .7f
        );
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(BotPauseLayer::onBotMode)
        );
        btn->setTag(static_cast<int>(mode));
        return btn;
    }

    void rebuildBotMenu() {
        auto& s = bot::state();
        m_fields->menu->removeAllChildren();
        m_fields->menu->addChild(this->makeBotButton("Record", bot::Mode::Record));
        m_fields->menu->addChild(this->makeBotButton("Off", bot::Mode::Off));
        m_fields->menu->addChild(this->makeBotButton("Play", bot::Mode::Play));
        m_fields->menu->updateLayout();

        m_fields->label->setString(
            fmt::format("GD Bot: {} | {} inputs", bot::modeName(s.mode), s.macro.size()).c_str()
        );
    }

    void onBotMode(CCObject* sender) {
        auto& s = bot::state();
        auto pl = PlayLayer::get();
        if (!pl) return;

        auto mode = static_cast<bot::Mode>(static_cast<CCNode*>(sender)->getTag());
        if (mode == s.mode) return;

        if (s.mode == bot::Mode::Record) bot::save();

        if (mode == bot::Mode::Play && s.macro.empty()) {
            FLAlertLayer::create("GD Bot", "Macro is empty. Record one first.", "OK")->show();
            return;
        }

        s.mode = mode;

        if (mode == bot::Mode::Off) {
            this->rebuildBotMenu();
            return;
        }

        // Record / Play: перезапускаем уровень с начала, чтобы счётчик шагов совпадал с макросом.
        // (Практические чекпоинты при этом сбрасываются.)
        if (mode == bot::Mode::Record) s.macro.clear();
        pl->resetLevelFromStart();
        this->onResume(nullptr);
    }
};
