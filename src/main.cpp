// GD Bot — macro recorder / replayer for Geometry Dash 2.2081 (Geode)
//
// Модель работы:
//  * Макрос = список событий {frame, button, down, player1}, frame — номер физического шага.
//  * Запись: хук handleButton пишет нажатия/отпускания с текущим номером шага.
//  * Воспроизведение: перед каждым шагом (processCommands) инжектим события этого шага.
//  * Практика: при загрузке чекпоинта счётчик шагов откатывается, запись обрезается.
//  * Запись: после каждого возрождения игра замирает на 0.15 с РЕАЛЬНОГО времени.
//  * Слоты макросов на уровень, асинхронное сохранение.
//
// ИСПРАВЛЕНИЯ В ЭТОЙ ВЕРСИИ (поверх предыдущих):
//  A) КРАШ: удалён кэшируемый m_fields->tpsLabel (висячий указатель после
//     removeAllChildren()). Лейбл TPS создаётся заново при каждой пересборке.
//  B) КРАШ: пересборка меню из колбэка кнопки идёт через rebuildKeeping(sender)
//     (retain/autorelease), чтобы не уничтожить кнопку во время её activate().
//  C) Попапы модальные: глотают тачи (ccTouchBegan), имеют свой приоритет,
//     меню/TextInput внутри — приоритетом выше слоя. Back-кнопка закрывает попап.
//  D) Закрытие попапов через closeSelf() (retain/remove/autorelease) —
//     колбэк не работает с уже освобождённым this.
//  E) Колбэки попапов держат Ref на владельца (нет dangling this).
//  F) Список макросов обновляется с задержкой после "Save as" (запись асинхронная,
//     файл ещё мог не появиться на диске).
//
// ИЗВЕСТНОЕ ОГРАНИЧЕНИЕ: «TPS» здесь — только fixed-step аккумулятор поверх
// GJBaseGameLayer::update(). Движок сам режет dt на свои шаги, поэтому реальная
// частота физики этим не меняется. Для настоящего TPS нужна правка шага внутри игры.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace bot {

    enum class Mode { Off = 0, Record = 1, Play = 2 };

    constexpr int RESPAWN_FREEZE_MS = 150;
    constexpr uint32_t MAX_MACRO_INPUTS = 10'000'000;

    inline constexpr uint32_t kTpsPresets[] = {
        60, 120, 240, 480, 960, 1000, 1440, 2000, 3000, 4000
    };

    struct Input {
        uint32_t frame;
        uint8_t button;
        bool down;
        bool p1;
        uint8_t held;
    };

    struct State {
        static constexpr uint32_t kMinTps = 60;
        static constexpr uint32_t kMaxTps = 4000;

        Mode mode = Mode::Off;
        uint32_t frame = 0;
        std::vector<Input> macro;
        size_t playIdx = 0;

        bool syncHeld = false;
        bool injecting = false;

        bool loadedFromCheckpoint = false;
        std::unordered_map<CheckpointObject*, uint32_t> cpFrames;
        std::string levelKey;

        std::string macroName = "default";

        uint8_t graceMask = 0;
        uint8_t finger = 0;
        uint8_t gameHeld = 0;

        bool isFrozen = false;
        std::chrono::steady_clock::time_point freezeEndTime{};

        bool inResetLevel = false;
        int cpLoadDepth = 0;
        bool cpLoadedInReset = false;

        bool panelOpen = false;

        uint32_t tps = 240;
        double tpsAccumulator = 0.0;        // остаток времени getModifiedDelta (в игровых секундах)

        // Замер РЕАЛЬНОГО TPS (сколько раз в секунду вызывается processCommands).
        uint32_t measureCount = 0;
        std::chrono::steady_clock::time_point measureStart{};
        uint32_t measuredTps = 0;
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool isPlayLayer(GJBaseGameLayer* gl) {
        return gl != nullptr && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == gl;
    }

    inline uint8_t bitOf(bool p1, int button) {
        if (button < 0 || button > 3) return 0;
        return static_cast<uint8_t>(1u << ((p1 ? 0 : 4) + button));
    }

    // ---------- TPS ----------

    inline void setTps(uint32_t v) {
        auto& s = state();
        v = std::clamp(v, State::kMinTps, State::kMaxTps);
        s.tps = v;
        s.tpsAccumulator = 0.0;
    }

    inline void resetMeasure() {
        auto& s = state();
        s.measureCount = 0;
        s.measureStart = std::chrono::steady_clock::now();
    }

    inline void countStep() {
        auto& s = state();
        ++s.measureCount;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - s.measureStart).count();

        if (elapsed >= 1.0) {
            s.measuredTps = static_cast<uint32_t>(s.measureCount / elapsed + 0.5);
            s.measureCount = 0;
            s.measureStart = now;
        }
    }

    inline void cycleTps(int dir) {
        auto& s = state();
        constexpr size_t n = sizeof(kTpsPresets) / sizeof(kTpsPresets[0]);

        if (dir > 0) {
            for (size_t i = 0; i < n; ++i) {
                if (kTpsPresets[i] > s.tps) { setTps(kTpsPresets[i]); return; }
            }
            setTps(kTpsPresets[n - 1]);
        } else {
            for (size_t i = n; i-- > 0; ) {
                if (kTpsPresets[i] < s.tps) { setTps(kTpsPresets[i]); return; }
            }
            setTps(kTpsPresets[0]);
        }
    }

    // ---------- работа с макросом ----------

    inline uint8_t heldNow() {
        auto& m = state().macro;
        return m.empty() ? 0 : m.back().held;
    }

    inline bool push(uint32_t f, uint8_t button, bool down, bool p1) {
        auto& s = state();

        uint8_t m = bitOf(p1, button);
        if (!m) return false;

        uint8_t cur = heldNow();
        bool isHeld = (cur & m) != 0;

        if (isHeld == down) return false;

        cur = down ? (cur | m) : (cur & ~m);

        s.macro.push_back({f, button, down, p1, cur});
        return true;
    }

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
        s.isFrozen = false;
    }

    inline void applyHeldMask(GJBaseGameLayer* gl, uint8_t target) {
        auto& s = state();

        uint8_t cur = s.gameHeld;
        if (cur == target) return;

        bool wasInjecting = s.injecting;
        s.injecting = true;

        for (int p = 0; p < 2; ++p) {
            bool p1 = (p == 0);

            for (int b = 0; b < 4; ++b) {
                uint8_t bit = bitOf(p1, b);

                if ((cur & ~target) & bit) {
                    gl->handleButton(false, b, p1);
                }
                if ((target & ~cur) & bit) {
                    gl->handleButton(true, b, p1);
                }
            }
        }

        s.gameHeld = target;
        s.injecting = wasInjecting;
    }

    // Конец окна возрождения (gl == nullptr — просто закрыть окно).
    // Разбираются оба источника: macroHeld (graceMask) и fingerHeld (s.finger).
    inline void endGrace(GJBaseGameLayer* gl) {
        auto& s = state();

        uint8_t mask = s.graceMask;
        s.graceMask = 0;
        s.isFrozen = false;

        for (int p = 0; p < 2; ++p) {
            bool p1 = (p == 0);

            for (int b = 0; b < 4; ++b) {
                uint8_t bit = bitOf(p1, b);

                bool macroHeld = (mask & bit) != 0;
                bool fingerHeld = (s.finger & bit) != 0;

                if (fingerHeld) {
                    if (gl && !(s.gameHeld & bit)) {
                        bool wasInjecting = s.injecting;
                        s.injecting = true;

                        gl->handleButton(true, b, p1);
                        s.gameHeld |= bit;

                        s.injecting = wasInjecting;
                    }

                    if (!macroHeld) {
                        push(s.frame, static_cast<uint8_t>(b), true, p1);
                    }
                } else if (macroHeld) {
                    if (gl && (s.gameHeld & bit)) {
                        bool wasInjecting = s.injecting;
                        s.injecting = true;

                        gl->handleButton(false, b, p1);
                        s.gameHeld &= static_cast<uint8_t>(~bit);

                        s.injecting = wasInjecting;
                    }

                    push(s.frame, static_cast<uint8_t>(b), false, p1);
                }
            }
        }
    }

    inline void seek(uint32_t f) {
        auto& s = state();

        s.frame = f;
        s.playIdx = static_cast<size_t>(
            std::lower_bound(
                s.macro.begin(), s.macro.end(), f,
                [](Input const& i, uint32_t v) { return i.frame < v; }
            ) - s.macro.begin()
        );

        s.syncHeld = true;
        s.gameHeld = 0;
        s.tpsAccumulator = 0.0;
    }

    inline void resetToStart() {
        auto& s = state();

        seek(0);

        s.graceMask = 0;
        s.isFrozen = false;

        if (s.mode == Mode::Record) {
            truncateFrom(0);
        }
    }

    inline void inject(GJBaseGameLayer* gl) {
        auto& s = state();

        uint8_t target = 0;

        if (s.playIdx > 0 && s.playIdx <= s.macro.size()) {
            target = s.macro[s.playIdx - 1].held;
        }

        size_t n = s.macro.size();

        while (s.playIdx < n && s.macro[s.playIdx].frame <= s.frame) {
            Input const& e = s.macro[s.playIdx++];

            uint8_t m = bitOf(e.p1, e.button);
            if (!m) continue;

            if (e.down) {
                target |= m;
            } else {
                target &= static_cast<uint8_t>(~m);
            }
        }

        s.syncHeld = false;
        applyHeldMask(gl, target);
    }

    // ---------- сохранение / загрузка ----------

    inline std::filesystem::path macrosRootDir() {
        return Mod::get()->getSaveDir() / "macros";
    }

    inline std::filesystem::path macroDir() {
        return macrosRootDir() / state().levelKey;
    }

    inline std::filesystem::path macroPath(std::string const& name) {
        return macroDir() / (name + ".gdbm");
    }

    inline std::mutex& saveMutex() {
        static std::mutex m;
        return m;
    }

    inline std::vector<char> serializeMacro(std::vector<Input> const& macro, uint32_t tpsForSave) {
        uint32_t version = 2;
        uint32_t count = static_cast<uint32_t>(macro.size());
        uint32_t tps = tpsForSave;

        std::vector<char> buf(16 + static_cast<size_t>(count) * 6);

        std::memcpy(buf.data(), "GDBM", 4);
        std::memcpy(buf.data() + 4, &version, 4);
        std::memcpy(buf.data() + 8, &count, 4);
        std::memcpy(buf.data() + 12, &tps, 4);

        char* p = buf.data() + 16;

        for (auto const& e : macro) {
            uint8_t flags = static_cast<uint8_t>((e.down ? 1 : 0) | (e.p1 ? 2 : 0));

            std::memcpy(p, &e.frame, 4);
            p[4] = static_cast<char>(e.button);
            p[5] = static_cast<char>(flags);

            p += 6;
        }

        return buf;
    }

    inline bool writeMacroFile(std::filesystem::path const& path, std::vector<char> const& buf) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        auto tmpPath = path.string() + ".tmp";

        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            log::error("GDBot: can't open temp macro file for writing: {}", tmpPath);
            return false;
        }

        f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        f.close();

        std::filesystem::rename(tmpPath, path, ec);
        if (ec) {
            log::error("GDBot: failed to rename temp macro file");
            return false;
        }

        return true;
    }

    inline void saveSlotSync(std::filesystem::path const& path, std::vector<Input> const& macroCopy, uint32_t tps) {
        auto buf = serializeMacro(macroCopy, tps);

        std::lock_guard<std::mutex> lock(saveMutex());
        if (writeMacroFile(path, buf)) {
            log::info("GDBot: saved {} inputs to '{}'", macroCopy.size(), path.filename().string());
        }
    }

    inline void save() {
        auto& s = state();
        if (s.levelKey.empty() || s.macroName.empty()) return;

        auto path = macroPath(s.macroName);
        auto macroCopy = s.macro;
        uint32_t tps = s.tps;

        std::thread([path, tps, macroCopy = std::move(macroCopy)]() mutable {
            saveSlotSync(path, macroCopy, tps);
        }).detach();
    }

    inline void saveAs(std::string const& name) {
        if (name.empty()) return;
        state().macroName = name;
        save();
    }

    inline void load(std::string const& name) {
        auto& s = state();

        s.macro.clear();
        s.macroName = name.empty() ? "default" : name;

        if (s.levelKey.empty()) return;

        std::error_code ec;
        auto path = macroPath(s.macroName);

        if (!std::filesystem::exists(path, ec)) return;

        auto size = std::filesystem::file_size(path, ec);
        if (ec || size < 12) return;

        if (size > 16 + static_cast<uint64_t>(MAX_MACRO_INPUTS) * 6) {
            log::warn("GDBot: macro file too large, ignoring");
            return;
        }

        std::ifstream f(path, std::ios::binary);
        if (!f) return;

        std::vector<char> buf(static_cast<size_t>(size));
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));

        if (!f) return;

        uint32_t version = 0;
        uint32_t count = 0;

        std::memcpy(&version, buf.data() + 4, 4);
        std::memcpy(&count, buf.data() + 8, 4);

        if (std::memcmp(buf.data(), "GDBM", 4) != 0 || (version != 1 && version != 2)) return;

        // v1: заголовок 12 байт, TPS не хранится (макрос писался при 240).
        // v2: заголовок 16 байт, в конце заголовка — TPS записи.
        size_t headerSize = (version == 2) ? 16 : 12;
        if (size < headerSize) return;
        if (static_cast<uint64_t>(count) * 6 + headerSize != size) return;

        uint32_t fileTps = 240;
        if (version == 2) {
            std::memcpy(&fileTps, buf.data() + 12, 4);
        }

        // Макрос привязан к номерам физических шагов, поэтому воспроизводить
        // его нужно на том же TPS, на котором он записан.
        setTps(fileTps);

        s.macro.reserve(static_cast<size_t>(count) + 1024);

        char const* p = buf.data() + headerSize;

        uint32_t lastFrame = 0;
        size_t loadedCount = 0;

        for (uint32_t i = 0; i < count; ++i, p += 6) {
            uint32_t frame = 0;
            std::memcpy(&frame, p, 4);

            if (frame < lastFrame) {
                log::warn("GDBot: macro file is not sorted by frame, stopping load");
                break;
            }

            lastFrame = frame;

            uint8_t button = static_cast<uint8_t>(p[4]);
            if (button > 3) continue;

            uint8_t flags = static_cast<uint8_t>(p[5]);
            bool down = (flags & 1) != 0;
            bool p1 = (flags & 2) != 0;

            if (push(frame, button, down, p1)) {
                ++loadedCount;
            }
        }

        log::info("GDBot: loaded {} inputs (slot '{}', {} records)", loadedCount, s.macroName, count);
    }

    inline void load() {
        load(state().macroName);
    }

    struct MacroSlotInfo {
        std::string name;
        size_t count;
    };

    inline std::vector<MacroSlotInfo> listSlots() {
        std::vector<MacroSlotInfo> result;

        auto& s = state();
        if (s.levelKey.empty()) return result;

        std::error_code ec;
        auto dir = macroDir();

        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
            return result;
        }

        for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;

            auto const& path = entry.path();
            if (path.extension() != ".gdbm") continue;

            size_t count = 0;

            std::ifstream f(path, std::ios::binary);
            if (f) {
                char hdr[12];
                f.read(hdr, 12);

                if (f.gcount() == 12 && std::memcmp(hdr, "GDBM", 4) == 0) {
                    uint32_t c = 0;
                    std::memcpy(&c, hdr + 8, 4);
                    count = c;
                }
            }

            result.push_back({ path.stem().string(), count });
        }

        std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
            return a.name < b.name;
        });

        return result;
    }

    inline bool deleteSlot(std::string const& name) {
        std::error_code ec;
        return std::filesystem::remove(macroPath(name), ec);
    }

    inline void migrateLegacyMacro() {
        auto& s = state();

        std::error_code ec;
        auto legacyPath = macrosRootDir() / (s.levelKey + ".gdbm");

        if (!std::filesystem::exists(legacyPath, ec)) return;

        std::filesystem::create_directories(macroDir(), ec);
        auto newPath = macroPath("default");

        if (!std::filesystem::exists(newPath, ec)) {
            std::filesystem::rename(legacyPath, newPath, ec);
            if (!ec) {
                log::info("GDBot: migrated legacy macro file to slot 'default'");
            }
        }
    }

    inline void onEnterLevel(GJGameLevel* level) {
        auto& s = state();

        s.mode = Mode::Off;
        s.frame = 0;
        s.playIdx = 0;

        s.syncHeld = false;
        s.injecting = false;

        s.loadedFromCheckpoint = false;
        s.inResetLevel = false;
        s.cpLoadDepth = 0;
        s.cpLoadedInReset = false;

        s.panelOpen = false;
        s.finger = 0;
        s.gameHeld = 0;

        s.isFrozen = false;
        s.freezeEndTime = {};

        s.tpsAccumulator = 0.0;

        s.cpFrames.clear();
        clearGrace();

        int id = level->m_levelID.value();

        if (id > 0) {
            s.levelKey = std::to_string(id);
        } else {
            std::string name = level->m_levelName;

            for (auto& c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
                    c = '_';
                }
            }

            s.levelKey = "local_" + name;
        }

        s.macroName = "default";
        migrateLegacyMacro();
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

        if (!bot::isPlayLayer(this)) {
            GJBaseGameLayer::update(dt);
            return;
        }

        if (s.isFrozen) {
            if (s.mode != bot::Mode::Record) {
                s.isFrozen = false;
                bot::clearGrace();
            } else {
                if (std::chrono::steady_clock::now() >= s.freezeEndTime) {
                    bot::endGrace(this);
                } else {
                    return;
                }
            }
        }

        // Реальная частота физики задаётся хуком getModifiedDelta (ниже).
        GJBaseGameLayer::update(dt);
    }

    // Подменяет квантование времени движка (1/240 с) на 1/tps. Остаток времени
    // копится в s.tpsAccumulator, поэтому физика не зависит от FPS рендера.
    // Вне PlayLayer (редактор) поведение оригинальное.
    float getModifiedDelta(float dt) {
        auto& s = bot::state();

        if (!bot::isPlayLayer(this)) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }

        double warp = std::max<double>(m_gameState.m_timeWarp, 0.01);
        double spt = 1.0 / static_cast<double>(std::max<uint32_t>(s.tps, bot::State::kMinTps));

        double total = static_cast<double>(dt) * warp + s.tpsAccumulator;

        // Защита от "spiral of death" после долгого фриза/лага.
        double maxTotal = spt * std::max<uint32_t>(8u, s.tps / 4u);
        if (total > maxTotal) total = maxTotal;

        double steps = std::floor(total / spt + 1e-9);
        double newDelta = steps * spt;

        s.tpsAccumulator = total - newDelta;

        return static_cast<float>(newDelta);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = bot::state();
        bool inPlay = bot::isPlayLayer(this);

        if (
            inPlay &&
            s.mode == bot::Mode::Play &&
            (
                s.syncHeld ||
                (s.playIdx < s.macro.size() && s.macro[s.playIdx].frame <= s.frame)
            )
        ) {
            bot::inject(this);
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (inPlay) {
            s.frame++;
            bot::countStep();
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = bot::state();

        const bool gameButton = (button >= 0 && button <= 3);

        if (!s.injecting && bot::isPlayLayer(this)) {
            if (
                s.mode == bot::Mode::Play &&
                gameButton &&
                (s.playIdx < s.macro.size() || s.syncHeld)
            ) {
                return;
            }

            if (s.mode == bot::Mode::Record && gameButton) {
                // Служебные вызовы движка во время reset/загрузки чекпоинта
                // не являются реальным вводом игрока.
                bool isEngineReset = s.inResetLevel || (s.cpLoadDepth > 0);

                if (!isEngineReset) {
                    uint8_t m = bot::bitOf(isPlayer1, button);

                    if (down) {
                        s.finger |= m;
                    } else {
                        s.finger &= static_cast<uint8_t>(~m);
                    }

                    if (s.isFrozen) {
                        return;
                    }

                    bot::push(s.frame, static_cast<uint8_t>(button), down, isPlayer1);
                }
            }
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!s.injecting && bot::isPlayLayer(this) && gameButton) {
            uint8_t m = bot::bitOf(isPlayer1, button);

            if (m) {
                if (down) {
                    s.gameHeld |= m;
                } else {
                    s.gameHeld &= static_cast<uint8_t>(~m);
                }
            }
        }
    }
};

// =====================================================================
//  Уровень: сбросы, чекпоинты
// =====================================================================

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        bot::onEnterLevel(level);
        return true;
    }

    void resetLevel() {
        auto& s = bot::state();

        if (s.inResetLevel || s.cpLoadDepth > 0) {
            PlayLayer::resetLevel();
            return;
        }

        s.inResetLevel = true;
        s.cpLoadedInReset = false;

        PlayLayer::resetLevel();

        if (!s.cpLoadedInReset) {
            bot::resetToStart();
            s.cpFrames.clear();

            if (s.mode == bot::Mode::Record) {
                s.isFrozen = true;
                s.freezeEndTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(bot::RESPAWN_FREEZE_MS);
            }
        }

        s.inResetLevel = false;
    }

    void resetLevelFromStart() {
        auto& s = bot::state();

        if (s.cpLoadDepth > 0) {
            PlayLayer::resetLevelFromStart();
            return;
        }

        bool wasInResetLevel = s.inResetLevel;
        s.inResetLevel = true;

        PlayLayer::resetLevelFromStart();

        s.inResetLevel = wasInResetLevel;

        s.cpFrames.clear();
        bot::resetToStart();
    }

    void storeCheckpoint(CheckpointObject* cp) {
        PlayLayer::storeCheckpoint(cp);
        bot::state().cpFrames[cp] = bot::state().frame;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        auto& s = bot::state();

        ++s.cpLoadDepth;
        s.cpLoadedInReset = true;

        PlayLayer::loadFromCheckpoint(cp);

        --s.cpLoadDepth;

        auto it = s.cpFrames.find(cp);

        if (it == s.cpFrames.end()) {
            log::warn("GDBot: unknown checkpoint, restarting macro from the beginning");

            bot::resetToStart();
            s.cpFrames.clear();

            if (s.mode == bot::Mode::Record) {
                s.isFrozen = true;
                s.freezeEndTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(bot::RESPAWN_FREEZE_MS);
            }

            return;
        }

        uint32_t f = it->second;

        bot::seek(f);

        if (s.mode == bot::Mode::Record) {
            bot::truncateFrom(f);
            s.graceMask = bot::heldNow();

            s.isFrozen = true;
            s.freezeEndTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(bot::RESPAWN_FREEZE_MS);
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
//  Модальные окна бота
//
//  Приоритеты касаний (меньше число = выше приоритет):
//    слой попапа  : base      — глотает всё, что не попало в кнопки
//    меню/ввод    : base - 1  — получают тач раньше слоя
//  Prompt открывается поверх Macro-попапа, поэтому у него base ниже.
// =====================================================================

namespace botui {
    constexpr int kMacroPopupPrio  = -9000;
    constexpr int kPromptPopupPrio = -9100;
}

class BotPromptPopup : public cocos2d::CCLayerColor {
protected:
    geode::TextInput* m_input = nullptr;
    std::function<void(std::string)> m_callback;

    bool init(std::string const& title, std::string const& placeholder, std::function<void(std::string)> cb) {
        auto winSize = CCDirector::get()->getWinSize();

        if (!CCLayerColor::initWithColor(ccc4(0, 0, 0, 160), winSize.width, winSize.height)) {
            return false;
        }

        this->setPosition({0.f, 0.f});
        this->setZOrder(10000);

        // Модальность: слой глотает все касания.
        this->setTouchMode(kCCTouchesOneByOne);
        this->setTouchPriority(botui::kPromptPopupPrio);
        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);

        m_callback = std::move(cb);

        float const panelW = 260.f;
        float const panelH = 140.f;
        float const px = (winSize.width - panelW) / 2.f;
        float const py = (winSize.height - panelH) / 2.f;

        auto panel = CCLayerColor::create(ccc4(20, 20, 30, 255), panelW, panelH);
        panel->setPosition({px, py});
        this->addChild(panel);

        auto titleLabel = CCLabelBMFont::create(title.c_str(), "bigFont.fnt");
        titleLabel->setScale(.6f);
        titleLabel->setPosition({panelW / 2.f, panelH - 22.f});
        panel->addChild(titleLabel);

        m_input = geode::TextInput::create(panelW - 40.f, placeholder);
        m_input->setPosition({panelW / 2.f, panelH / 2.f + 6.f});
        panel->addChild(m_input);

        // Поле ввода должно получать тач раньше затемняющего слоя.
        if (auto node = m_input->getInputNode()) {
            node->setTouchPriority(botui::kPromptPopupPrio - 1);
        }

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setHandlerPriority(botui::kPromptPopupPrio - 1);
        panel->addChild(menu);

        auto okSpr = ButtonSprite::create("OK", "bigFont.fnt", "GJ_button_01.png", .8f);
        auto okBtn = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(BotPromptPopup::onOk));
        okBtn->setPosition({panelW / 2.f + 45.f, 24.f});
        menu->addChild(okBtn);

        auto cancelSpr = ButtonSprite::create("Cancel", "bigFont.fnt", "GJ_button_06.png", .8f);
        auto cancelBtn = CCMenuItemSpriteExtra::create(cancelSpr, this, menu_selector(BotPromptPopup::onCancel));
        cancelBtn->setPosition({panelW / 2.f - 45.f, 24.f});
        menu->addChild(cancelBtn);

        return true;
    }

    // Безопасное закрытие из колбэка собственной кнопки.
    void closeSelf() {
        this->retain();
        this->removeFromParentAndCleanup(true);
        this->autorelease();
    }

public:
    bool ccTouchBegan(CCTouch*, CCEvent*) override { return true; }
    void keyBackClicked() override { this->closeSelf(); }

    void onOk(CCObject*) {
        std::string value = m_input ? std::string(m_input->getString()) : std::string();
        auto cb = m_callback;

        this->closeSelf();

        if (cb) {
            cb(value);
        }
    }

    void onCancel(CCObject*) {
        this->closeSelf();
    }

    static BotPromptPopup* create(
        std::string title,
        std::string placeholder,
        std::function<void(std::string)> cb
    ) {
        auto ret = new BotPromptPopup();

        if (ret->init(title, placeholder, std::move(cb))) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }

    void show() {
        auto scene = CCDirector::get()->getRunningScene();
        if (scene) {
            scene->addChild(this, 10000);
        }
    }
};

class BotMacroPopup : public cocos2d::CCLayerColor {
protected:
    CCLayerColor* m_panel = nullptr;
    CCMenu* m_listMenu = nullptr;
    std::vector<std::string> m_names;

    bool init() {
        auto winSize = CCDirector::get()->getWinSize();

        if (!CCLayerColor::initWithColor(ccc4(0, 0, 0, 160), winSize.width, winSize.height)) {
            return false;
        }

        this->setPosition({0.f, 0.f});
        this->setZOrder(10000);

        this->setTouchMode(kCCTouchesOneByOne);
        this->setTouchPriority(botui::kMacroPopupPrio);
        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);

        float const panelW = 360.f;
        float const panelH = 260.f;
        float const px = (winSize.width - panelW) / 2.f;
        float const py = (winSize.height - panelH) / 2.f;

        m_panel = CCLayerColor::create(ccc4(20, 20, 30, 255), panelW, panelH);
        m_panel->setPosition({px, py});
        this->addChild(m_panel);

        auto titleLabel = CCLabelBMFont::create("GD Bot Macros", "bigFont.fnt");
        titleLabel->setScale(.65f);
        titleLabel->setPosition({panelW / 2.f, panelH - 20.f});
        m_panel->addChild(titleLabel);

        m_listMenu = CCMenu::create();
        m_listMenu->setPosition({0.f, 0.f});
        m_listMenu->setHandlerPriority(botui::kMacroPopupPrio - 1);
        m_panel->addChild(m_listMenu);

        auto saveAsSpr = ButtonSprite::create("Save current as...", "bigFont.fnt", "GJ_button_04.png", .7f);
        saveAsSpr->setScale(.6f);
        auto saveAsBtn = CCMenuItemSpriteExtra::create(saveAsSpr, this, menu_selector(BotMacroPopup::onSaveAs));
        saveAsBtn->setPosition({panelW / 2.f - 70.f, 20.f});

        auto closeSpr = ButtonSprite::create("Close", "bigFont.fnt", "GJ_button_06.png", .7f);
        closeSpr->setScale(.6f);
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSpr, this, menu_selector(BotMacroPopup::onCloseClicked));
        closeBtn->setPosition({panelW / 2.f + 70.f, 20.f});

        auto bottomMenu = CCMenu::create();
        bottomMenu->addChild(saveAsBtn);
        bottomMenu->addChild(closeBtn);
        bottomMenu->setPosition({0.f, 0.f});
        bottomMenu->setHandlerPriority(botui::kMacroPopupPrio - 1);
        m_panel->addChild(bottomMenu);

        this->rebuildList();
        return true;
    }

    void closeSelf() {
        this->retain();
        this->removeFromParentAndCleanup(true);
        this->autorelease();
    }

    void rebuildList() {
        // Кнопки в списке: колбэк кнопки Del вызывает rebuildList(), поэтому
        // удаляем детей, удерживая сам popup от освобождения.
        this->retain();

        m_listMenu->removeAllChildren();

        // Старые лейблы строк (дети m_panel, не m_listMenu).
        // В Geode v5 CCARRAY_FOREACH удалён — используем CCArrayExt.
        CCArray* children = m_panel->getChildren();
        if (children) {
            std::vector<CCNode*> toRemove;

            for (auto node : CCArrayExt<CCNode*>(children)) {
                if (node->getTag() == 12345) {
                    toRemove.push_back(node);
                }
            }

            for (auto* n : toRemove) {
                n->removeFromParentAndCleanup(true);
            }
        }

        m_names.clear();

        auto slots = bot::listSlots();
        auto panelSize = m_panel->getContentSize();

        float cx = panelSize.width / 2.f;
        float topY = panelSize.height - 55.f;

        constexpr int kMaxVisible = 6;
        int shown = std::min<int>(kMaxVisible, static_cast<int>(slots.size()));

        for (int i = 0; i < shown; ++i) {
            auto const& slot = slots[static_cast<size_t>(i)];
            int tag = static_cast<int>(m_names.size());
            m_names.push_back(slot.name);

            auto label = CCLabelBMFont::create(
                fmt::format("{} ({})", slot.name, slot.count).c_str(),
                "chatFont.fnt"
            );
            label->setScale(.55f);
            label->setAnchorPoint({0.f, .5f});
            label->setTag(12345);
            label->setPosition({16.f, topY - i * 28.f});
            m_panel->addChild(label);

            auto loadSpr = ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_01.png", .6f);
            loadSpr->setScale(.5f);
            auto loadBtn = CCMenuItemSpriteExtra::create(loadSpr, this, menu_selector(BotMacroPopup::onLoad));
            loadBtn->setTag(tag);
            loadBtn->setPosition({cx + 60.f, topY - i * 28.f});
            m_listMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("Del", "bigFont.fnt", "GJ_button_06.png", .6f);
            delSpr->setScale(.5f);
            auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(BotMacroPopup::onDelete));
            delBtn->setTag(tag);
            delBtn->setPosition({cx + 108.f, topY - i * 28.f});
            m_listMenu->addChild(delBtn);
        }

        if (slots.empty()) {
            auto label = CCLabelBMFont::create("No saved macros yet", "chatFont.fnt");
            label->setScale(.6f);
            label->setTag(12345);
            label->setPosition({cx, topY});
            m_panel->addChild(label);
        }

        this->autorelease();
    }

public:
    bool ccTouchBegan(CCTouch*, CCEvent*) override { return true; }
    void keyBackClicked() override { this->closeSelf(); }

    // Вызывается через CCCallFunc после асинхронного сохранения.
    void onRefresh() {
        this->rebuildList();
    }

    void onLoad(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        if (tag < 0 || tag >= static_cast<int>(m_names.size())) return;

        bot::load(m_names[static_cast<size_t>(tag)]);

        auto pl = PlayLayer::get();
        auto& s = bot::state();

        // Закрываемся ДО сброса уровня: дальше код может перестроить сцену.
        this->closeSelf();

        if (pl && (s.mode == bot::Mode::Play || s.mode == bot::Mode::Record)) {
            pl->resetLevelFromStart();
        }
    }

    void onDelete(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        if (tag < 0 || tag >= static_cast<int>(m_names.size())) return;

        bot::deleteSlot(m_names[static_cast<size_t>(tag)]);

        // sender будет уничтожен внутри rebuildList(), пока выполняется его
        // же activate() — удерживаем до конца колбэка.
        sender->retain();
        this->rebuildList();
        sender->autorelease();
    }

    void onSaveAs(CCObject*) {
        Ref<BotMacroPopup> self(this);

        auto popup = BotPromptPopup::create(
            "Save macro as",
            "name",
            [self](std::string name) {
                if (name.empty()) return;

                bot::saveAs(name);

                // Запись идёт в фоне — даём файлу появиться и обновляем список.
                self->runAction(CCSequence::create(
                    CCDelayTime::create(0.3f),
                    CCCallFunc::create(self, callfunc_selector(BotMacroPopup::onRefresh)),
                    nullptr
                ));
            }
        );

        if (popup) popup->show();
    }

    void onCloseClicked(CCObject*) {
        this->closeSelf();
    }

    static BotMacroPopup* create() {
        auto ret = new BotMacroPopup();

        if (ret->init()) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }

    void show() {
        auto scene = CCDirector::get()->getRunningScene();
        if (scene) {
            scene->addChild(this, 10000);
        }
    }
};

// =====================================================================
//  Меню паузы
// =====================================================================

class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCMenu* menu = nullptr;
        CCMenu* panel = nullptr;
        CCLabelBMFont* label = nullptr;
        // tpsLabel намеренно отсутствует: это ребёнок panel, который
        // удаляется в removeAllChildren(); сырой указатель висел бы.
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();

        float const x = 95.f;
        float const topY = win.height * 0.62f;

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu);
        m_fields->menu = menu;

        auto toggleSpr = ButtonSprite::create("Bot", "bigFont.fnt", "GJ_button_04.png", .7f);
        toggleSpr->setScale(.7f);

        auto toggle = CCMenuItemSpriteExtra::create(
            toggleSpr,
            this,
            menu_selector(BotPauseLayer::onBotToggle)
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
        this->addChild(m_fields->label);

        this->rebuildBotMenu();
    }

    CCMenuItemSpriteExtra* makeBotButton(char const* text, bot::Mode mode) {
        bool selected = bot::state().mode == mode;

        auto spr = ButtonSprite::create(
            text,
            "bigFont.fnt",
            selected ? "GJ_button_01.png" : "GJ_button_04.png",
            .7f
        );

        spr->setScale(.6f);

        auto btn = CCMenuItemSpriteExtra::create(
            spr,
            this,
            menu_selector(BotPauseLayer::onBotMode)
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

        // --- Строка TPS: [-] [ 1000 ] [+]  [Set] ---
        float tpsRowY = topY - 26.f * 4;

        auto decSpr = ButtonSprite::create("-", "bigFont.fnt", "GJ_button_04.png", .7f);
        decSpr->setScale(.6f);
        auto decBtn = CCMenuItemSpriteExtra::create(decSpr, this, menu_selector(BotPauseLayer::onTpsDec));
        decBtn->setPosition({x - 55.f, tpsRowY});
        panel->addChild(decBtn);

        auto incSpr = ButtonSprite::create("+", "bigFont.fnt", "GJ_button_04.png", .7f);
        incSpr->setScale(.6f);
        auto incBtn = CCMenuItemSpriteExtra::create(incSpr, this, menu_selector(BotPauseLayer::onTpsInc));
        incBtn->setPosition({x - 20.f, tpsRowY});
        panel->addChild(incBtn);

        auto setSpr = ButtonSprite::create("Set", "bigFont.fnt", "GJ_button_04.png", .6f);
        setSpr->setScale(.55f);
        auto setBtn = CCMenuItemSpriteExtra::create(setSpr, this, menu_selector(BotPauseLayer::onTpsCustom));
        setBtn->setPosition({x + 55.f, tpsRowY});
        panel->addChild(setBtn);

        // Лейбл создаётся заново при каждой пересборке — никаких висячих указателей.
        auto tpsLabel = CCLabelBMFont::create(fmt::format("{}", s.tps).c_str(), "bigFont.fnt");
        tpsLabel->setScale(.4f);
        tpsLabel->setPosition({x + 14.f, tpsRowY});
        panel->addChild(tpsLabel);

        // --- Менеджер макросов ---
        auto macrosSpr = ButtonSprite::create("Macros...", "bigFont.fnt", "GJ_button_04.png", .7f);
        macrosSpr->setScale(.6f);
        auto macrosBtn = CCMenuItemSpriteExtra::create(macrosSpr, this, menu_selector(BotPauseLayer::onOpenMacros));
        macrosBtn->setPosition({x, topY - 26.f * 5});
        panel->addChild(macrosBtn);

        m_fields->label->setPosition({x, topY - 26.f * 6 - 20.f});
        m_fields->label->setString(
            fmt::format(
                "GD Bot: {}\nSlot: {} ({} inputs)\nTPS: {} (measured: {})",
                bot::modeName(s.mode),
                s.macroName,
                s.macro.size(),
                s.tps,
                s.measuredTps
            ).c_str()
        );

        panel->setVisible(s.panelOpen);
        m_fields->label->setVisible(s.panelOpen);
    }

    // Пересборка меню из колбэка кнопки: удерживаем sender, пока его
    // activate() ещё выполняется, иначе removeAllChildren() его освободит.
    void rebuildKeeping(CCObject* sender) {
        if (sender) sender->retain();
        this->rebuildBotMenu();
        if (sender) sender->autorelease();
    }

    void onBotToggle(CCObject* sender) {
        auto& s = bot::state();
        s.panelOpen = !s.panelOpen;
        this->rebuildKeeping(sender);
    }

    void onTpsDec(CCObject* sender) {
        bot::cycleTps(-1);
        this->rebuildKeeping(sender);
    }

    void onTpsInc(CCObject* sender) {
        bot::cycleTps(+1);
        this->rebuildKeeping(sender);
    }

    void onTpsCustom(CCObject*) {
        // Ref удерживает слой паузы, пока открыт попап.
        Ref<CCObject> keep(this);

        auto popup = BotPromptPopup::create(
            "Custom TPS",
            fmt::format("{}", bot::state().tps),
            [this, keep](std::string value) {
                if (value.empty()) return;

                std::string digits;
                for (char c : value) {
                    if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
                }
                if (digits.empty()) return;

                uint32_t v = 0;
                try {
                    v = static_cast<uint32_t>(std::stoul(digits));
                } catch (...) {
                    return;
                }

                bot::setTps(v);
                this->rebuildBotMenu(); // вызов из попапа — sender не нужен
            }
        );

        if (popup) popup->show();
    }

    void onOpenMacros(CCObject*) {
        auto popup = BotMacroPopup::create();
        if (popup) popup->show();
    }

    void onResume(CCObject* sender) {
        PauseLayer::onResume(sender);

        auto& s = bot::state();
        bot::resetMeasure();

        if (s.mode == bot::Mode::Play) {
            s.syncHeld = true;
        }
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
            FLAlertLayer::create(
                "GD Bot",
                "Macro is empty. Record one first.",
                "OK"
            )->show();

            return;
        }

        s.mode = mode;

        bot::clearGrace();

        s.finger = 0;
        s.gameHeld = 0;

        if (mode == bot::Mode::Off) {
            this->rebuildKeeping(sender);
            return;
        }

        if (mode == bot::Mode::Record) {
            s.macro.clear();
            s.macro.reserve(std::max<size_t>(1u << 14, static_cast<size_t>(s.tps) * 60));
        }

        pl->resetLevelFromStart();
        this->onResume(nullptr);
    }
};
