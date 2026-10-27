// GD Bot — macro recorder / replayer for Geometry Dash 2.2081 (Geode)
//
// Модель работы:
//  * Макрос = список событий {frame, button, down, player1}, где frame — номер физического шага.
//  * Запись: хук GJBaseGameLayer::handleButton пишет нажатия/отпускания с текущим номером шага.
//    Дубли (нажатие уже зажатой кнопки / отпускание уже отпущенной) в макрос не попадают.
//  * Воспроизведение: перед каждым шагом (processCommands) инжектим события этого шага.
//    Реальный ввод блокируется, пока в макросе есть непроигранные события; после конца — управление игроку.
//  * Практика: при загрузке чекпоинта счётчик шагов откатывается на шаг чекпоинта,
//    а запись обрезается до этого шага — дальше она перезаписывается.
//  * Запись: после каждого возрождения игра замирает на 0.15 с РЕАЛЬНОГО времени.
//    Время заморозки не зависит от скорости игры и от TPS.
//  * TPS: частота физических шагов задаётся ботом (не зависит от FPS рендера) через
//    fixed-step аккумулятор поверх GJBaseGameLayer::update(). Поддерживается от 60 до 4000.
//  * Оптимизация воспроизведения/записи: события одного кадра схлопываются в итоговую маску
//    зажатых кнопок, в игру отправляются только реально изменившиеся нажатия/отпускания.
//    Это критично при высоком TPS и спам-кликах — без схлопывания игра бы захлёбывалась
//    количеством вызовов handleButton.
//  * Макросы хранятся слотами (именованными файлами) на уровень — можно сохранить несколько
//    записей под разными именами и переключаться между ними через меню паузы.
//  * Сохранение на диск асинхронное (в фоновом потоке) — большой макрос (миллионы событий
//    при высоком TPS) не подвешивает игру в момент levelComplete()/onQuit().
//
// ---------------------------------------------------------------------
// ИСПРАВЛЕНИЯ / ДОПОЛНЕНИЯ В ЭТОЙ ВЕРСИИ:
//
//  1) endGrace(): ресинк зажатых кнопок после заморозки проходит по ОБОИМ источникам —
//     macroHeld (что держал макрос на момент чекпоинта) И fingerHeld (что физически
//     зажато игроком прямо сейчас), разбирая все 4 комбинации. Раньше кнопки, которых
//     не было в graceMask, полностью терялись — это "съедало" зажатия дэш-орбов.
//
//  2) resetLevelFromStart(): обёрнут в s.inResetLevel, как и resetLevel(), чтобы вложенный
//     вызов resetLevel() изнутри оригинала не дублировал логику бота.
//
//  3) handleButton(): пока мы находимся ВНУТРИ служебного resetLevel()/resetLevelFromStart()/
//     loadFromCheckpoint() (флаги inResetLevel / cpLoadDepth), входящие вызовы handleButton
//     считаются служебными вызовами движка (игра может сама "отпустить" кнопку визуально
//     при смерти/рестарте ещё до того, как бот выставит graceMask) и НЕ трогают s.finger
//     и макрос. Благодаря этому, если игрок физически держит дэш-орб через смерть/чекпоинт,
//     зажатие не прерывается — s.finger остаётся true, и endGrace() корректно продолжает
//     удержание без разрыва на "отпустил-нажал".
//
//  4) Добавлен настраиваемый TPS (fixed-step аккумулятор), многослотовое сохранение/загрузка
//     макросов и асинхронное сохранение — см. комментарии в соответствующих местах.
// ---------------------------------------------------------------------

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
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

    constexpr int RESPAWN_FREEZE_MS = 150; // 0.15 секунды реального времени
    constexpr uint32_t MAX_MACRO_INPUTS = 10'000'000;

    // Пресеты TPS для кнопок +/- в меню.
    inline constexpr uint32_t kTpsPresets[] = {
        60, 120, 240, 480, 960, 1000, 1440, 2000, 3000, 4000
    };

    struct Input {
        uint32_t frame;
        uint8_t button;
        bool down;
        bool p1;
        uint8_t held;   // маска зажатых кнопок ПОСЛЕ этого события (не сохраняется в файл)
    };

    struct State {
        static constexpr uint32_t kMinTps = 60;
        static constexpr uint32_t kMaxTps = 4000;

        Mode mode = Mode::Off;
        uint32_t frame = 0;                 // номер текущего физического шага
        std::vector<Input> macro;           // всегда отсортирован по frame
        size_t playIdx = 0;                 // следующее событие для воспроизведения

        bool syncHeld = false;              // нужно восстановить зажатые кнопки (после перемотки / паузы)
        bool injecting = false;             // сейчас вводим события сами

        bool loadedFromCheckpoint = false;  // legacy flag, оставлен для совместимости логики
        std::unordered_map<CheckpointObject*, uint32_t> cpFrames;
        std::string levelKey;

        // Текущий "слот" (имя файла) макроса для этого уровня.
        std::string macroName = "default";

        uint8_t graceMask = 0;              // кнопки, зажатые в макросе на момент чекпоинта
        uint8_t finger = 0;                 // какие кнопки сейчас реально зажаты игроком
        uint8_t gameHeld = 0;               // какие кнопки сейчас зажаты в самой игре по состоянию бота

        // Реальное время заморозки после возрождения
        bool isFrozen = false;
        std::chrono::steady_clock::time_point freezeEndTime{};

        // Защита от реентерабельности при сбросе/загрузке чекпоинта
        bool inResetLevel = false;
        int cpLoadDepth = 0;
        bool cpLoadedInReset = false;

        bool panelOpen = false;             // раскрыто ли меню бота в паузе

        // --- TPS ---
        uint32_t tps = 240;                 // целевая частота физических шагов (шагов/сек)
        double tpsAccumulator = 0.0;        // накопитель fixed-step по игровому dt
    };

    inline State& state() {
        static State s;
        return s;
    }

    inline bool isPlayLayer(GJBaseGameLayer* gl) {
        return gl != nullptr && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == gl;
    }

    // бит кнопки в маске: P1 -> биты 0..3, P2 -> биты 4..7
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

    // добавить событие; возвращает false, если оно ничего не меняет (дубль)
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

    // удалить все события с frame >= f
    inline void truncateFrom(uint32_t f) {
        auto& s = state();

        auto it = std::lower_bound(
            s.macro.begin(),
            s.macro.end(),
            f,
            [](Input const& i, uint32_t v) {
                return i.frame < v;
            }
        );

        s.macro.erase(it, s.macro.end());
    }

    inline void clearGrace() {
        auto& s = state();
        s.graceMask = 0;
        s.isFrozen = false;
    }

    // Применяет маску зажатых кнопок, отправляя события ТОЛЬКО для изменившихся битов.
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

                // Было зажато, должно быть отпущено
                if ((cur & ~target) & bit) {
                    gl->handleButton(false, b, p1);
                }

                // Не было зажато, должно быть зажато
                if ((target & ~cur) & bit) {
                    gl->handleButton(true, b, p1);
                }
            }
        }

        s.gameHeld = target;
        s.injecting = wasInjecting;
    }

    // ---------------------------------------------------------------
    // Конец окна возрождения.
    //
    // gl == nullptr — просто закрыть окно без вмешательства в игру
    // (например, при levelComplete()/onQuit()).
    //
    // Разбираются ОБА источника:
    //   mask       — что держал МАКРОС на момент чекпоинта/рестарта;
    //   s.finger   — что ФИЗИЧЕСКИ зажато игроком прямо сейчас (с учётом того,
    //                что служебные вызовы движка во время сброса/загрузки
    //                чекпоинта НЕ трогают s.finger — см. handleButton()).
    //
    //   macroHeld=1, fingerHeld=1 -> держим дальше, в макрос новое событие
    //                                не пишем — это продолжение удержания.
    //   macroHeld=1, fingerHeld=0 -> палец отпущен за время заморозки:
    //                                отпускаем в игре и пишем "up" в макрос.
    //   macroHeld=0, fingerHeld=1 -> кнопка зажата заново:
    //                                зажимаем в игре и пишем "down" в макрос.
    //   macroHeld=0, fingerHeld=0 -> ничего не делаем.
    // ---------------------------------------------------------------
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

    // Перемотка счётчика шагов и указателя воспроизведения на шаг f.
    inline void seek(uint32_t f) {
        auto& s = state();

        s.frame = f;
        s.playIdx = static_cast<size_t>(
            std::lower_bound(
                s.macro.begin(),
                s.macro.end(),
                f,
                [](Input const& i, uint32_t v) {
                    return i.frame < v;
                }
            ) - s.macro.begin()
        );

        s.syncHeld = true;
        s.gameHeld = 0;

        // Сбрасываем накопитель TPS, чтобы реальное время, потраченное на
        // перемотку/загрузку, не вылилось в лавину "догоняющих" шагов.
        s.tpsAccumulator = 0.0;
    }

    // Сброс уровня на самое начало.
    inline void resetToStart() {
        auto& s = state();

        seek(0);

        s.graceMask = 0;
        s.isFrozen = false;

        if (s.mode == Mode::Record) {
            truncateFrom(0);
        }
    }

    // Инжекция событий текущего шага (схлопнуты в маску — см. шапку файла).
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

    // ---------- сохранение / загрузка (многослотовое) ----------

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

    inline std::vector<char> serializeMacro(std::vector<Input> const& macro) {
        uint32_t version = 1;
        uint32_t count = static_cast<uint32_t>(macro.size());

        std::vector<char> buf(12 + static_cast<size_t>(count) * 6);

        std::memcpy(buf.data(), "GDBM", 4);
        std::memcpy(buf.data() + 4, &version, 4);
        std::memcpy(buf.data() + 8, &count, 4);

        char* p = buf.data() + 12;

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

    // Синхронная запись слота (используется из фонового потока и для миграции).
    inline void saveSlotSync(std::filesystem::path const& path, std::vector<Input> const& macroCopy) {
        auto buf = serializeMacro(macroCopy);

        std::lock_guard<std::mutex> lock(saveMutex());
        if (writeMacroFile(path, buf)) {
            log::info("GDBot: saved {} inputs to '{}'", macroCopy.size(), path.filename().string());
        }
    }

    // Асинхронное сохранение текущего макроса (s.macroName) для текущего уровня.
    // Копируем вектор СРАЗУ в основном потоке и уводим сериализацию+запись в фоновый
    // поток, чтобы большой макрос (миллионы событий при высоком TPS) не подвешивал
    // игру в момент levelComplete()/onQuit()/переключения режима.
    inline void save() {
        auto& s = state();
        if (s.levelKey.empty() || s.macroName.empty()) return;

        auto path = macroPath(s.macroName);
        auto macroCopy = s.macro;

        std::thread([path, macroCopy = std::move(macroCopy)]() mutable {
            saveSlotSync(path, macroCopy);
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

        if (size > 12 + static_cast<uint64_t>(MAX_MACRO_INPUTS) * 6) {
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

        if (std::memcmp(buf.data(), "GDBM", 4) != 0 || version != 1) return;
        if (static_cast<uint64_t>(count) * 6 + 12 != size) return;

        s.macro.reserve(static_cast<size_t>(count) + 1024);

        char const* p = buf.data() + 12;

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
            if (button > 3) {
                continue;
            }

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

    // Миграция из старого плоского формата (<levelKey>.gdbm) в новый слот "default".
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
        // s.tps сознательно НЕ сбрасываем — настройка TPS сохраняется между уровнями
        // в рамках сессии игры.

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
            // Не игровой слой (например, редактор) — TPS-степпинг бота не нужен.
            GJBaseGameLayer::update(dt);
            return;
        }

        // Заморозка после возрождения: ровно 0.15 с РЕАЛЬНОГО времени,
        // независимо от TPS и скорости игры.
        if (s.isFrozen) {
            if (s.mode != bot::Mode::Record) {
                s.isFrozen = false;
                bot::clearGrace();
            } else {
                if (std::chrono::steady_clock::now() >= s.freezeEndTime) {
                    bot::endGrace(this);
                    // проваливаемся ниже — в этом же вызове можно начать считать физику
                } else {
                    return; // не даём игре тикать, пока идёт заморозка
                }
            }
        }

        // -------------------------------------------------------------
        // Fixed-step аккумулятор: частота физических шагов (TPS) задаётся
        // ботом (s.tps) и НЕ зависит от FPS рендера. За один визуальный
        // кадр мы можем прогнать 0, 1 или несколько физических шагов —
        // это и позволяет писать/воспроизводить макросы на 1000/2000+
        // шагов в секунду, даже если экран рендерится на 60/144 Гц.
        //
        // maxSteps защищает от "spiral of death": если устройство не
        // успевает считать физику с заданным TPS, лишнее накопленное
        // время сбрасывается вместо попытки "нагнать" его разом большим
        // количеством шагов за один кадр (что само по себе вызвало бы
        // ещё более сильный лаг/зависание).
        // -------------------------------------------------------------
        double stepDt = 1.0 / static_cast<double>(std::max<uint32_t>(s.tps, bot::State::kMinTps));

        s.tpsAccumulator += static_cast<double>(dt);

        uint32_t maxSteps = std::max<uint32_t>(8u, s.tps / 4u); // не более ~0.25с "долга" за кадр
        uint32_t steps = 0;

        while (s.tpsAccumulator >= stepDt && steps < maxSteps) {
            GJBaseGameLayer::update(static_cast<float>(stepDt));
            s.tpsAccumulator -= stepDt;
            ++steps;

            // Чекпоинт/смерть могли включить заморозку посреди "догоняющего"
            // цикла — прерываемся, остаток докрутим в следующих кадрах.
            if (s.isFrozen) {
                s.tpsAccumulator = 0.0;
                break;
            }
        }

        if (steps >= maxSteps) {
            s.tpsAccumulator = 0.0;
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = bot::state();
        bool inPlay = bot::isPlayLayer(this);

        // Быстрый путь: инжектим только если есть что восстанавливать или проигрывать.
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
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = bot::state();

        const bool gameButton = (button >= 0 && button <= 3);

        if (!s.injecting && bot::isPlayLayer(this)) {
            // Во время воспроизведения блокируем только игровые кнопки 0..3.
            // Системные кнопки, включая паузу, должны продолжать работать.
            if (
                s.mode == bot::Mode::Play &&
                gameButton &&
                (s.playIdx < s.macro.size() || s.syncHeld)
            ) {
                return;
            }

            if (s.mode == bot::Mode::Record && gameButton) {
                // Пока мы внутри служебного сброса уровня (resetLevel/
                // resetLevelFromStart) или загрузки чекпоинта, игра может
                // САМА вызвать handleButton(false, ...), чтобы визуально
                // снять удержание кнопки (например, отпустить дэш-орб) ещё
                // до того, как мы выставим graceMask/заморозку. Это НЕ
                // реальное отпускание пальца — физическая клавиша/тач всё
                // ещё зажаты, и новое "нажатие" от ОС не придёт. Такие
                // служебные вызовы не должны менять s.finger и не должны
                // попадать в макрос.
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
//  Попапы: ввод текста/числа и менеджер макросов
//
//  ВНИМАНИЕ: сигнатуры geode::Popup<...>::initAnchored / setTitle и
//  geode::TextInput::create / getString соответствуют распространённому
//  в комьюнити паттерну Geode-попапов. Если в вашей версии SDK они
//  немного отличаются — поправьте эти несколько мест, вся остальная
//  (игровая) логика бота от этого не зависит.
// =====================================================================

class BotPromptPopup : public geode::Popup<std::string, std::string, std::function<void(std::string)>> {
protected:
    geode::TextInput* m_input = nullptr;
    std::function<void(std::string)> m_callback;

    bool setup(std::string title, std::string placeholder, std::function<void(std::string)> cb) override {
        this->setTitle(title.c_str());
        m_callback = cb;

        auto size = m_mainLayer->getContentSize();

        m_input = geode::TextInput::create(200.f, placeholder);
        m_input->setPosition({ size.width / 2.f, size.height / 2.f + 6.f });
        m_mainLayer->addChild(m_input);

        auto okSpr = ButtonSprite::create("OK", "bigFont.fnt", "GJ_button_01.png", .8f);
        auto okBtn = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(BotPromptPopup::onOk));
        okBtn->setPosition({ 0.f, -34.f });

        auto menu = CCMenu::create();
        menu->addChild(okBtn);
        menu->setPosition({ size.width / 2.f, size.height / 2.f });
        m_mainLayer->addChild(menu);

        return true;
    }

public:
    void onOk(CCObject*) {
        std::string value = m_input ? m_input->getString() : std::string();

        if (m_callback) {
            m_callback(value);
        }

        this->onClose(nullptr);
    }

    static BotPromptPopup* create(
        std::string title,
        std::string placeholder,
        std::function<void(std::string)> cb
    ) {
        auto ret = new BotPromptPopup();

        if (ret->initAnchored(240.f, 140.f, title, placeholder, cb)) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }
};

class BotMacroPopup : public geode::Popup<> {
protected:
    CCMenu* m_listMenu = nullptr;
    std::vector<std::string> m_names;

    bool setup() override {
        this->setTitle("GD Bot Macros");

        m_listMenu = CCMenu::create();
        m_listMenu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(m_listMenu);

        auto size = m_mainLayer->getContentSize();

        auto saveAsSpr = ButtonSprite::create("Save current as...", "bigFont.fnt", "GJ_button_04.png", .7f);
        saveAsSpr->setScale(.6f);
        auto saveAsBtn = CCMenuItemSpriteExtra::create(saveAsSpr, this, menu_selector(BotMacroPopup::onSaveAs));

        auto bottomMenu = CCMenu::create();
        bottomMenu->addChild(saveAsBtn);
        bottomMenu->setPosition({ size.width / 2.f, 22.f });
        m_mainLayer->addChild(bottomMenu);

        this->rebuildList();
        return true;
    }

    void rebuildList() {
        m_listMenu->removeAllChildren();

        // Удаляем старые текстовые лейблы строк (они дети m_mainLayer, не m_listMenu).
        CCObject* obj;
        CCArray* children = m_mainLayer->getChildren();
        if (children) {
            std::vector<CCNode*> toRemove;
            CCARRAY_FOREACH(children, obj) {
                auto node = static_cast<CCNode*>(obj);
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
        auto size = m_mainLayer->getContentSize();

        float cx = size.width / 2.f;
        float topY = size.height - 50.f;

        constexpr int kMaxVisible = 6;
        int shown = std::min<int>(kMaxVisible, static_cast<int>(slots.size()));

        for (int i = 0; i < shown; ++i) {
            auto const& slot = slots[i];
            int tag = static_cast<int>(m_names.size());
            m_names.push_back(slot.name);

            auto label = CCLabelBMFont::create(
                fmt::format("{} ({})", slot.name, slot.count).c_str(),
                "chatFont.fnt"
            );
            label->setScale(.55f);
            label->setAnchorPoint({ 0.f, .5f });
            label->setTag(12345);
            label->setPosition({ 16.f, topY - i * 28.f });
            m_mainLayer->addChild(label);

            auto loadSpr = ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_01.png", .6f);
            loadSpr->setScale(.5f);
            auto loadBtn = CCMenuItemSpriteExtra::create(loadSpr, this, menu_selector(BotMacroPopup::onLoad));
            loadBtn->setTag(tag);
            loadBtn->setPosition({ cx + 60.f, topY - i * 28.f });
            m_listMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("Del", "bigFont.fnt", "GJ_button_06.png", .6f);
            delSpr->setScale(.5f);
            auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(BotMacroPopup::onDelete));
            delBtn->setTag(tag);
            delBtn->setPosition({ cx + 108.f, topY - i * 28.f });
            m_listMenu->addChild(delBtn);
        }

        if (slots.empty()) {
            auto label = CCLabelBMFont::create("No saved macros yet", "chatFont.fnt");
            label->setScale(.6f);
            label->setTag(12345);
            label->setPosition({ cx, topY });
            m_mainLayer->addChild(label);
        }
    }

    void onLoad(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        if (tag < 0 || tag >= static_cast<int>(m_names.size())) return;

        bot::load(m_names[static_cast<size_t>(tag)]);

        auto pl = PlayLayer::get();
        auto& s = bot::state();

        if (pl && (s.mode == bot::Mode::Play || s.mode == bot::Mode::Record)) {
            pl->resetLevelFromStart();
        }

        this->onClose(nullptr);
    }

    void onDelete(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        if (tag < 0 || tag >= static_cast<int>(m_names.size())) return;

        bot::deleteSlot(m_names[static_cast<size_t>(tag)]);
        this->rebuildList();
    }

    void onSaveAs(CCObject*) {
        auto popup = BotPromptPopup::create(
            "Save macro as",
            "name",
            [this](std::string name) {
                if (name.empty()) return;
                bot::saveAs(name);
                this->rebuildList();
            }
        );

        if (popup) popup->show();
    }

public:
    static BotMacroPopup* create() {
        auto ret = new BotMacroPopup();

        if (ret->initAnchored(360.f, 260.f)) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }
};

// =====================================================================
//  Меню паузы: кнопка "Bot" слева -> раскрывает Record / Off / Play,
//  управление TPS и менеджер макросов.
// =====================================================================

class $modify(BotPauseLayer, PauseLayer) {
    struct Fields {
        CCMenu* menu = nullptr;             // кнопка-переключатель (всегда видна)
        CCMenu* panel = nullptr;            // кнопки режимов (раскрываются)
        CCLabelBMFont* label = nullptr;
        CCLabelBMFont* tpsLabel = nullptr;
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

        // --- Строка TPS: [-] [ 1000 ] [+]  [Set...] ---
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

        if (!m_fields->tpsLabel) {
            m_fields->tpsLabel = CCLabelBMFont::create("", "bigFont.fnt");
            m_fields->tpsLabel->setScale(.4f);
        }
        m_fields->tpsLabel->setString(fmt::format("{}", s.tps).c_str());
        m_fields->tpsLabel->setPosition({x + 14.f, tpsRowY});
        if (m_fields->tpsLabel->getParent() != panel) {
            if (m_fields->tpsLabel->getParent()) {
                m_fields->tpsLabel->removeFromParentAndCleanup(false);
            }
            panel->addChild(m_fields->tpsLabel);
        }

        // --- Кнопка менеджера макросов ---
        auto macrosSpr = ButtonSprite::create("Macros...", "bigFont.fnt", "GJ_button_04.png", .7f);
        macrosSpr->setScale(.6f);
        auto macrosBtn = CCMenuItemSpriteExtra::create(macrosSpr, this, menu_selector(BotPauseLayer::onOpenMacros));
        macrosBtn->setPosition({x, topY - 26.f * 5});
        panel->addChild(macrosBtn);

        m_fields->label->setPosition({x, topY - 26.f * 6 - 20.f});
        m_fields->label->setString(
            fmt::format(
                "GD Bot: {}\nSlot: {} ({} inputs)",
                bot::modeName(s.mode),
                s.macroName,
                s.macro.size()
            ).c_str()
        );

        panel->setVisible(s.panelOpen);
        m_fields->label->setVisible(s.panelOpen);
    }

    void onBotToggle(CCObject*) {
        auto& s = bot::state();
        s.panelOpen = !s.panelOpen;
        this->rebuildBotMenu();
    }

    void onTpsDec(CCObject*) {
        bot::cycleTps(-1);
        this->rebuildBotMenu();
    }

    void onTpsInc(CCObject*) {
        bot::cycleTps(+1);
        this->rebuildBotMenu();
    }

    void onTpsCustom(CCObject*) {
        auto popup = BotPromptPopup::create(
            "Custom TPS",
            fmt::format("{}", bot::state().tps),
            [this](std::string value) {
                if (value.empty()) return;

                // Оставляем только цифры на случай мусора во вводе.
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
                this->rebuildBotMenu();
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
            this->rebuildBotMenu();
            return;
        }

        if (mode == bot::Mode::Record) {
            s.macro.clear();
            // Резервируем ёмкость с запасом под ~60 секунд записи при текущем TPS,
            // чтобы избежать реаллокаций вектора (и связанных с ними микро-лагов)
            // в разгар записи на высоком TPS.
            s.macro.reserve(std::max<size_t>(1u << 14, static_cast<size_t>(s.tps) * 60));
        }

        pl->resetLevelFromStart();
        this->onResume(nullptr);
    }
};
