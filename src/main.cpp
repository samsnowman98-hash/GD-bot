// GD Bot — macro recorder / replayer for Geometry Dash 2.2081 (Geode)
//
// Модель работы:
//  * Макрос = список событий {frame, button, down, player1}, где frame — номер физического шага (240 TPS).
//  * Запись: хук GJBaseGameLayer::handleButton пишет нажатия/отпускания с текущим номером шага.
//    Дубли (нажатие уже зажатой кнопки / отпускание уже отпущенной) в макрос не попадают.
//  * Воспроизведение: перед каждым шагом (processCommands) инжектим события этого шага.
//    Реальный ввод блокируется, пока в макросе есть непроигранные события; после конца — управление игроку.
//  * Практика: при загрузке чекпоинта счётчик шагов откатывается на шаг чекпоинта,
//    а запись обрезается до этого шага — дальше она перезаписывается.
//  * Запись: после каждого возрождения игра замирает на 0.15 с реального времени.
//    Во время заморозки ввод в игру НЕ проходит и в макрос не пишется — только запоминается,
//    зажата ли кнопка. В конце заморозки:
//       - кнопка была зажата в макросе на чекпоинте И ты её держишь -> зажатие продолжается
//         без нового клика (в макросе ничего не добавляется);
//       - кнопка была зажата в макросе, но ты её не держишь -> в макрос пишется отпускание;
//       - любые другие нажатия во время заморозки игнорируются.
//  * Оптимизация: у каждого события хранится маска зажатых кнопок после него, поэтому
//    восстановление зажатий (пауза, чекпоинт) — O(1), а не проход по всему макросу;
//    файл читается/пишется одним блоком.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace bot {

    enum class Mode { Off = 0, Record = 1, Play = 2 };

    constexpr float RESPAWN_FREEZE = 0.15f; // секунд реального времени

    struct Input {
        uint32_t frame;
        uint8_t button;
        bool down;
        bool p1;
        uint8_t held;   // маска зажатых кнопок ПОСЛЕ этого события (не сохраняется в файл)
    };

    struct State {
        Mode mode = Mode::Off;
        uint32_t frame = 0;                 // номер текущего физического шага
        std::vector<Input> macro;           // всегда отсортирован по frame
        size_t playIdx = 0;                 // следующее событие для воспроизведения
        bool syncHeld = false;              // нужно восстановить зажатые кнопки (после перемотки / паузы)
        bool injecting = false;             // сейчас вводим события сами
        bool loadedFromCheckpoint = false;  // resetLevel загрузил чекпоинт
        std::unordered_map<CheckpointObject*, uint32_t> cpFrames;
        std::string levelKey;

        uint8_t graceMask = 0;              // кнопки, зажатые в макросе на момент чекпоинта
        uint8_t finger = 0;                 // какие кнопки сейчас реально зажаты игроком
        float freezeLeft = 0.f;             // сколько осталось заморозки после возрождения

        bool panelOpen = false;             // раскрыто ли меню бота в паузе
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool isPlayLayer(GJBaseGameLayer* gl) {
        return gl != nullptr && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == gl;
    }

    // бит кнопки в маске: P1 -> биты 0..3, P2 -> биты 4..7 (0, если кнопка вне диапазона)
    inline uint8_t bitOf(bool p1, int button) {
        if (button < 0 || button > 3) return 0;
        return static_cast<uint8_t>(1u << ((p1 ? 0 : 4) + button));
    }

    // ---------- работа с макросом ----------

    inline uint8_t heldNow() {
        auto& m = state().macro;
        return m.empty() ? 0 : m.back().held;
    }

    // добавить событие; возвращает false, если оно ничего не меняет (дубль)
    inline bool push(uint32_t f, uint8_t button, bool down, bool p1) {
        auto& s = state();
        uint8_t cur = heldNow();
        uint8_t m = bitOf(p1, button);
        if (m) {
            bool isHeld = (cur & m) != 0;
            if (isHeld == down) return false;
            cur = down ? (cur | m) : (cur & ~m);
        }
        s.macro.push_back({f, button, down, p1, cur});
        return true;
    }

    // удалить все события с frame >= f
    inline void truncateFrom(uint32_t f) {
        auto& s = state();
        auto it = std::lower_bound(
            s.macro.begin(), s.macro.end(), f,
            [](Input const& i, uint32_t v) { return i.frame < v; }
        );
        s.macro.erase(it, s.macro.end());
    }

    inline void clearGrace() {
        auto& s = state();
        s.graceMask = 0;
        s.freezeLeft = 0.f;
    }

    // конец окна возрождения. gl == nullptr — просто закрыть окно без вмешательства в игру
    inline void endGrace(GJBaseGameLayer* gl) {
        auto& s = state();
        uint8_t mask = s.graceMask;
        s.graceMask = 0;
        s.freezeLeft = 0.f;

        for (int p = 0; p < 2; ++p) {
            for (int b = 0; b < 4; ++b) {
                uint8_t bit = bitOf(p == 0, b);
                if (!(mask & bit)) continue;

                if (s.finger & bit) {
                    // чистое продолжение зажатия: в макрос ничего не пишем,
                    // но игре кнопку зажимаем, чтобы состояние совпало с макросом
                    if (gl) {
                        s.injecting = true;
                        gl->handleButton(true, b, p == 0);
                        s.injecting = false;
                    }
                } else {
                    push(s.frame, static_cast<uint8_t>(b), false, p == 0);
                }
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
        s.graceMask = 0;
        if (s.mode == Mode::Record) truncateFrom(0);
    }

    // инжект событий текущего шага
    inline void inject(GJBaseGameLayer* gl) {
        auto& s = state();
        s.injecting = true;

        if (s.syncHeld) {
            s.syncHeld = false;
            // O(1): маска зажатых кнопок хранится в последнем уже проигранном событии
            uint8_t held = (s.playIdx > 0 && s.playIdx <= s.macro.size())
                ? s.macro[s.playIdx - 1].held : 0;
            for (int p = 0; p < 2; ++p) {
                for (int b = 0; b < 4; ++b) {
                    if (held & bitOf(p == 0, b)) gl->handleButton(true, b, p == 0);
                }
            }
        }

        auto const n = s.macro.size();
        while (s.playIdx < n && s.macro[s.playIdx].frame <= s.frame) {
            auto const& e = s.macro[s.playIdx++];
            gl->handleButton(e.down, e.button, e.p1);
        }

        s.injecting = false;
    }

    // ---------- сохранение / загрузка (одним блоком) ----------

    inline std::filesystem::path macroPath() {
        return Mod::get()->getSaveDir() / "macros" / (state().levelKey + ".gdbm");
    }

    inline void save() {
        auto& s = state();
        if (s.levelKey.empty()) return;

        std::error_code ec;
        std::filesystem::create_directories(Mod::get()->getSaveDir() / "macros", ec);

        uint32_t version = 1;
        uint32_t count = static_cast<uint32_t>(s.macro.size());

        std::vector<char> buf(12 + static_cast<size_t>(count) * 6);
        std::memcpy(buf.data(), "GDBM", 4);
        std::memcpy(buf.data() + 4, &version, 4);
        std::memcpy(buf.data() + 8, &count, 4);

        char* p = buf.data() + 12;
        for (auto const& e : s.macro) {
            uint8_t flags = (e.down ? 1 : 0) | (e.p1 ? 2 : 0);
            std::memcpy(p, &e.frame, 4);
            p[4] = static_cast<char>(e.button);
            p[5] = static_cast<char>(flags);
            p += 6;
        }

        std::ofstream f(macroPath(), std::ios::binary | std::ios::trunc);
        if (!f) {
            log::error("GDBot: can't open macro file for writing");
            return;
        }
        f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
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

        std::vector<char> buf(static_cast<size_t>(size));
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!f) return;

        uint32_t version = 0, count = 0;
        std::memcpy(&version, buf.data() + 4, 4);
        std::memcpy(&count, buf.data() + 8, 4);
        if (std::memcmp(buf.data(), "GDBM", 4) != 0 || version != 1) return;
        if (static_cast<uint64_t>(count) * 6 + 12 != size) return;

        s.macro.reserve(count + 1024);
        char const* p = buf.data() + 12;
        for (uint32_t i = 0; i < count; ++i, p += 6) {
            uint32_t frame;
            std::memcpy(&frame, p, 4);
            uint8_t button = static_cast<uint8_t>(p[4]);
            uint8_t flags = static_cast<uint8_t>(p[5]);
            push(frame, button, flags & 1, (flags & 2) != 0);
        }
        log::info("GDBot: loaded {} inputs", s.macro.size());
    }

    inline void onEnterLevel(GJGameLevel* level) {
        auto& s = state();
        s.mode = Mode::Off;
        s.frame = 0;
        s.playIdx = 0;
        s.syncHeld = false;
        s.injecting = false;
        s.panelOpen = false;
        s.finger = 0;
        s.cpFrames.clear();
        clearGrace();

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
    void update(float dt) {
        auto& s = bot::state();

        // заморозка после возрождения (только при записи), реальное время — не зависит от скорости игры
        if (s.freezeLeft > 0.f && bot::isPlayLayer(this)) {
            if (s.mode != bot::Mode::Record) {
                bot::clearGrace();
            } else {
                s.freezeLeft -= dt;
                if (s.freezeLeft <= 0.f) bot::endGrace(this);
                return;
            }
        }

        GJBaseGameLayer::update(dt);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = bot::state();
        bool inPlay = bot::isPlayLayer(this);

        // быстрый путь: вызываем inject только если есть что вводить
        if (inPlay && s.mode == bot::Mode::Play &&
            (s.syncHeld || (s.playIdx < s.macro.size() && s.macro[s.playIdx].frame <= s.frame))) {
            bot::inject(this);
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (inPlay) s.frame++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = bot::state();
        if (!s.injecting && bot::isPlayLayer(this)) {
            // воспроизведение: реальный ввод блокируем, пока в макросе остались события;
            // когда макрос закончился — управление у игрока
            if (s.mode == bot::Mode::Play && s.playIdx < s.macro.size()) return;

            if (s.mode == bot::Mode::Record) {
                // запоминаем реальное состояние пальца
                uint8_t m = bot::bitOf(isPlayer1, button);
                if (down) s.finger |= m; else s.finger &= ~m;

                // во время заморозки после возрождения ввод не проходит ни в игру, ни в макрос
                if (s.freezeLeft > 0.f) return;

                bot::push(s.frame, static_cast<uint8_t>(button), down, isPlayer1);
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

        // при записи: короткая заморозка после возрождения
        if (s.mode == bot::Mode::Record) s.freezeLeft = bot::RESPAWN_FREEZE;
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
            bot::truncateFrom(f);               // всё после чекпоинта перезаписывается
            s.graceMask = bot::heldNow();       // что макрос держит зажатым на чекпоинте
        }
    }

    void levelComplete() {
        if (bot::state().mode == bot::Mode::Record) {
            bot::endGrace(nullptr);
            bot::save();
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (bot::state().mode == bot::Mode::Record) {
            bot::endGrace(nullptr);
            bot::save();
        }
        bot::state().mode = bot::Mode::Off;
        bot::clearGrace();
        PlayLayer::onQuit();
    }
};

// =====================================================================
//  Меню паузы: кнопка "Bot" слева -> раскрывает Record / Off / Play
// =====================================================================

class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCMenu* menu = nullptr;     // кнопка-переключатель (всегда видна)
        CCMenu* panel = nullptr;    // кнопки режимов (раскрываются)
        CCLabelBMFont* label = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();

        // позиции подобраны под пустую область слева (справа — ползунки, снизу — клавиши)
        float const x = 95.f;
        float const topY = win.height * 0.62f;

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu);
        m_fields->menu = menu;

        auto toggleSpr = ButtonSprite::create("Bot", "bigFont.fnt", "GJ_button_04.png", .7f);
        toggleSpr->setScale(.7f);
        auto toggle = CCMenuItemSpriteExtra::create(
            toggleSpr, this, menu_selector(BotPauseLayer::onBotToggle)
        );
        toggle->setPosition({x, topY});
        menu->addChild(toggle);

        auto panel = CCMenu::create();
        panel->setPosition({0.f, 0.f});
        this->addChild(panel);
        m_fields->panel = panel;

        m_fields->label = CCLabelBMFont::create("", "bigFont.fnt");
        m_fields->label->setScale(.32f);
        m_fields->label->setAlignment(kCCTextAlignmentCenter);
        m_fields->label->setPosition({x, topY - 26.f * 3 - 30.f});
        this->addChild(m_fields->label);

        this->rebuildBotMenu();
    }

    CCMenuItemSpriteExtra* makeBotButton(char const* text, bot::Mode mode) {
        bool selected = bot::state().mode == mode;
        auto spr = ButtonSprite::create(
            text, "bigFont.fnt",
            selected ? "GJ_button_01.png" : "GJ_button_04.png",
            .7f
        );
        spr->setScale(.6f);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(BotPauseLayer::onBotMode)
        );
        btn->setTag(static_cast<int>(mode));
        return btn;
    }

    void rebuildBotMenu() {
        auto& s = bot::state();
        auto win = CCDirector::get()->getWinSize();
        float const x = 95.f;
        float const topY = win.height * 0.62f;

        auto panel = m_fields->panel;
        panel->removeAllChildren();

        char const* names[3] = { "Record", "Off", "Play" };
        bot::Mode modes[3] = { bot::Mode::Record, bot::Mode::Off, bot::Mode::Play };
        for (int i = 0; i < 3; ++i) {
            auto btn = this->makeBotButton(names[i], modes[i]);
            btn->setPosition({x, topY - 26.f * (i + 1)});
            panel->addChild(btn);
        }

        m_fields->label->setString(
            fmt::format("GD Bot: {}\n{} inputs", bot::modeName(s.mode), s.macro.size()).c_str()
        );

        panel->setVisible(s.panelOpen);
        m_fields->label->setVisible(s.panelOpen);
    }

    void onBotToggle(CCObject*) {
        auto& s = bot::state();
        s.panelOpen = !s.panelOpen;
        this->rebuildBotMenu();
    }

    void onResume(CCObject* sender) {
        PauseLayer::onResume(sender);
        // при выходе из паузы восстанавливаем кнопки, которые бот держал зажатыми
        auto& s = bot::state();
        if (s.mode == bot::Mode::Play) s.syncHeld = true;
    }

    void onBotMode(CCObject* sender) {
        auto& s = bot::state();
        auto pl = PlayLayer::get();
        if (!pl) return;

        auto mode = static_cast<bot::Mode>(static_cast<CCNode*>(sender)->getTag());
        if (mode == s.mode) return;

        if (s.mode == bot::Mode::Record) {
            bot::endGrace(nullptr);
            bot::save();
        }

        if (mode == bot::Mode::Play && s.macro.empty()) {
            FLAlertLayer::create("GD Bot", "Macro is empty. Record one first.", "OK")->show();
            return;
        }

        s.mode = mode;
        bot::clearGrace();

        if (mode == bot::Mode::Off) {
            this->rebuildBotMenu();
            return;
        }

        // Record / Play: перезапускаем уровень с начала, чтобы счётчик шагов совпадал с макросом.
        // (Практические чекпоинты при этом сбрасываются.)
        if (mode == bot::Mode::Record) {
            s.macro.clear();
            s.macro.reserve(1 << 14);
        }
        pl->resetLevelFromStart();
        this->onResume(nullptr);
    }
};
