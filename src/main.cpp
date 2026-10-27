// GD Bot — macro recorder / replayer для Geometry Dash 2.2081 (Geode 5.x)
// =====================================================================
//
// МОДЕЛЬ
//  * Единые часы мода — счётчик ФИЗИЧЕСКИХ ШАГОВ (s.step), увеличивается
//    ровно один раз на каждый вызов processCommands(). И запись, и
//    воспроизведение, и чекпоинты живут в этой одной шкале.
//  * Макрос = отсортированный список {step, button, down, player1, held}.
//  * Запись: хук handleButton пишет переходы с текущим s.step.
//  * Воспроизведение: ПЕРЕД каждым шагом инжектим события этого шага
//    через маску удержаний (applyHeldMask) — ровно так же, как это делает
//    игра, поэтому орбы/кольца/пады срабатывают корректно.
//  * Практика: загрузка чекпоинта откатывает s.step и обрезает запись.
//  * После возрождения запись замирает на 150 мс РЕАЛЬНОГО времени, при
//    этом физически зажатые клавиши ПРОДОЛЖАЮТ ДЕЙСТВОВАТЬ в игре
//    (см. beginGrace/resolveGrace) и не пишутся в макрос.
//
// ---------------------------------------------------------------------
// ИСПРАВЛЕНО В ЭТОЙ ВЕРСИИ
// ---------------------------------------------------------------------
// КРАШИ:
//  1) onBotMode(): вызов onResume(nullptr), а ПОТОМ обращение к sender и
//     пересборка меню — PauseLayer к этому моменту уже удалён (use-after-free)
//     + nullptr в селектор кнопки. Теперь: сначала пересборка (с retain
//     sender), затем отложенный выход из паузы через scheduleOnce (0 мс).
//  2) PlayLayer::init(level, useReplay, dontCreateObjects) — хук зависел от
//     точной сигнатуры init в 2.2081 и, что хуже, onEnterLevel() вызывался на
//     КАЖДЫЙ onEnter (включая возврат из паузы) и сбрасывал макрос/слот.
//     Теперь инициализация уровня в PlayLayer::onEnter() + защита от
//     повторного входа в тот же самый PlayLayer.
//  3) onEnterLevel(): level->m_levelID.value() бросал исключение на уровне
//     без ID. Теперь значение достаётся безопасно.
//  4) Попапы полностью переписаны на Popup<> из Geode UI: модальность,
//     приоритеты тачей, Back/Escape, блокировка ввода — всё штатное.
//     Убраны ручные CCLayerColor с ccTouchBegan и «магические» приоритеты,
//     которые ломали нажатия кнопок и не закрывались по Escape.
//     Убраны retain/removeFromParentAndCleanup/autorelease-трюки
//     (closeSelf) — Popup закрывается штатно и безопасно из своего же колбэка.
//  5) rebuildList() удалял кнопки списка прямо внутри activate() той же
//     кнопки (Del). Теперь пересборка списка отложена на 0 мс.
//  6) «Save as» обновлял список через CCDelayTime(0.3) — гонка с фоновой
//     записью файла. Теперь статус сохранения атомарный, список обновляется
//     по факту завершения записи (опрос каждые 0.1 с).
//  7) Отсутствовала проверка имени слота: "a/b", "..", "" давали выход за
//     пределы каталога и падение на create_directories/rename. Добавлена
//     санитизация.
//  8) Все колбэки попапов больше не держат сырой this: Popup<> живёт, пока
//     открыт, а отложенные действия планируются на узлах, которые их
//     переживут (или отменяются в onClose).
//
// ФУНКЦИОНАЛЬНОСТЬ:
//  9) TPS теперь вводится вручную ЛЮБЫМ числом 1..100000 (поле ввода + пресеты),
//     кнопки +/- крутят пресеты в диапазоне 60..4000.
// 10) Менеджер макросов: пагинация (а не первые 6), отметка текущего слота,
//     подтверждение перезаписи и удаления, нормальные имена.
//
// ТОЧНОСТЬ ЗАПИСИ/ВОСПРОИЗВЕДЕНИЯ:
// 11) Единая шкала шагов (s.step) вместо frame+playIdx, рассинхрон которых
//     давал пропуск/дублирование первого ввода после чекпоинта.
// 12) Запись автоматически завершается и сохраняется на СМЕРТИ в обычном
//     режиме (раньше смерть писала мусор в хвост макроса, а после
//     resetLevel запись молча начиналась заново).
// 13) Воспроизведение: mask-before-events — состояние кнопок до событий
//     текущего шага берётся из накопленной маски, а не из поля .held
//     соседней записи (ошибка на стыке чекпоинтов и после seek).
// 14) После окончания макроса управление возвращается игроку (раньше ввод
//     глотался навсегда, если syncHeld залипал).
// 15) load() больше не проверяет размер файла ДО чтения заголовка и
//     корректно поддерживает v1/v2/v3. Файлы v1/v2 автоматически
//     переносятся в v3 при первом сохранении.
//
// ДЭШ-ОРБ НА ЗАПИСИ (главный запрос):
// 16) Окно возрождения (grace) больше НЕ разрешается внутри update() прямым
//     вызовом handleButton — до processCommands() нажатие не доходило до
//     системы ввода игрока корректно, орб не активировался и кубик падал.
//     Теперь разрешение grace происходит в хуке processCommands() ПЕРЕД
//     оригиналом — то есть ровно в той же точке, где инжектится ввод при
//     воспроизведении (которое работает правильно).
// 17) Маска grace теперь строится из РЕАЛЬНО зажатых клавиш (s.finger),
//     а не из хвоста макроса, и перед её применением состояние кнопок игры
//     принудительно сверяется (applyHeldMask(0) -> applyHeldMask(mask)),
//     так что gameHeld никогда не расходится с реальностью после
//     loadFromCheckpoint().
// 18) Вводы, отжатые и снова зажатые внутри 150 мс паузы, не рвут удержание:
//     пишется только итоговое состояние на первом шаге после паузы.
//
// ---------------------------------------------------------------------
// ОГРАНИЧЕНИЕ ПО TPS (честно)
// ---------------------------------------------------------------------
//  Реальная частота физики задаётся тем, как движок дробит
//  getModifiedDelta() на свои подшаги (1/240). Мы квантуем дельту на сетку
//  1/tps и копим остаток в аккумуляторе, поэтому количество ШАГОВ
//  processCommands() действительно растёт вместе с tps — но только пока
//  движок согласен дробить кадр на столько подшагов. На очень высоких tps
//  упрётесь в потолок движка. В меню показывается «measured» — реальное
//  число шагов в секунду; ориентируйтесь на него, а не на заданное число.
// =====================================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace bot {

    enum class Mode { Off = 0, Record = 1, Play = 2 };

    constexpr int RESPAWN_FREEZE_MS = 150;
    constexpr uint32_t MAX_MACRO_INPUTS = 10'000'000;

    // Защита от «spiral of death»: максимум физических шагов на один кадр.
    constexpr double MAX_STEPS_PER_FRAME = 64.0;

    inline constexpr uint32_t kTpsPresets[] = {
        60, 120, 240, 480, 960, 1000, 1440, 2000, 3000, 4000
    };

    struct Input {
        uint32_t step;
        uint8_t button;
        bool down;
        bool p1;
        uint8_t held;   // накопленная маска после этого события (для v1/v2 совместимости)
    };

    struct State {
        // Ручной ввод TPS: 1..100000.
        static constexpr uint32_t kMinTps = 1;
        static constexpr uint32_t kMaxTps = 100000;
        // Пресеты крутятся в более узком диапазоне.
        static constexpr uint32_t kPresetMin = 60;
        static constexpr uint32_t kPresetMax = 4000;

        Mode mode = Mode::Off;

        // ---- единые часы ----
        uint32_t step = 0;              // сколько processCommands() уже выполнено
        std::vector<Input> macro;       // отсортировано по step
        size_t playIdx = 0;             // первое ещё не проигранное событие

        bool syncHeld = false;          // нужно принудительно выставить маску
        bool injecting = false;         // мы сами вызываем handleButton

        std::unordered_map<CheckpointObject*, uint32_t> cpFrames;
        std::string levelKey;
        std::string macroName = "default";

        uint8_t graceMask = 0;          // что должно быть зажато после паузы
        uint8_t finger = 0;             // что физически зажато сейчас
        uint8_t gameHeld = 0;           // что, по нашим данным, зажато в игре

        bool isFrozen = false;
        std::chrono::steady_clock::time_point freezeEndTime{};

        bool inResetLevel = false;
        int cpLoadDepth = 0;
        bool cpLoadedInReset = false;

        bool panelOpen = false;

        uint32_t tps = 240;
        double tpsAccumulator = 0.0;    // остаток в игровых секундах

        // Замер РЕАЛЬНОЙ частоты шагов.
        uint32_t measureCount = 0;
        std::chrono::steady_clock::time_point measureStart{};
        uint32_t measuredTps = 0;

        // Инициализированный уровень (защита от повторного onEnter).
        void* enteredLayer = nullptr;
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

    inline char const* modeName(Mode m) {
        switch (m) {
            case Mode::Record: return "Recording";
            case Mode::Play:   return "Playing";
            default:           return "Off";
        }
    }

    // ---------- TPS ----------

    inline void setTps(uint32_t v) {
        auto& s = state();
        s.tps = std::clamp(v, State::kMinTps, State::kMaxTps);
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
        double elapsed = std::chrono::duration<double>(now - s.measureStart).count();

        if (elapsed >= 0.5) {
            s.measuredTps = static_cast<uint32_t>(s.measureCount / elapsed + 0.5);
            s.measureCount = 0;
            s.measureStart = now;
        }
    }

    // Крутит пресеты, но вокруг текущего значения (которое может быть ручным).
    inline void cycleTps(int dir) {
        auto& s = state();
        constexpr size_t n = sizeof(kTpsPresets) / sizeof(kTpsPresets[0]);

        uint32_t cur = std::clamp(s.tps, State::kPresetMin, State::kPresetMax);

        if (dir > 0) {
            for (size_t i = 0; i < n; ++i) {
                if (kTpsPresets[i] > cur) { setTps(kTpsPresets[i]); return; }
            }
            setTps(kTpsPresets[n - 1]);
        } else {
            for (size_t i = n; i-- > 0; ) {
                if (kTpsPresets[i] < cur) { setTps(kTpsPresets[i]); return; }
            }
            setTps(kTpsPresets[0]);
        }
    }

    inline std::optional<uint32_t> parseTps(std::string const& raw) {
        std::string digits;
        for (char c : raw) {
            if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
        }
        if (digits.empty() || digits.size() > 9) return std::nullopt;

        unsigned long v = 0;
        try {
            v = std::stoul(digits);
        } catch (...) {
            return std::nullopt;
        }
        if (v < State::kMinTps || v > State::kMaxTps) return std::nullopt;

        return static_cast<uint32_t>(v);
    }

    // ---------- работа с макросом ----------

    inline uint8_t heldNow() {
        auto& m = state().macro;
        return m.empty() ? 0 : m.back().held;
    }

    inline bool push(uint32_t step, uint8_t button, bool down, bool p1) {
        auto& s = state();

        uint8_t m = bitOf(p1, button);
        if (!m) return false;

        uint8_t cur = heldNow();
        if (((cur & m) != 0) == down) return false;   // состояние не изменилось

        cur = down ? static_cast<uint8_t>(cur | m)
                   : static_cast<uint8_t>(cur & ~m);

        if (s.macro.size() >= MAX_MACRO_INPUTS) return false;

        s.macro.push_back({step, button, down, p1, cur});
        return true;
    }

    inline size_t lowerBoundIdx(uint32_t step) {
        auto& m = state().macro;
        return static_cast<size_t>(
            std::lower_bound(
                m.begin(), m.end(), step,
                [](Input const& i, uint32_t v) { return i.step < v; }
            ) - m.begin()
        );
    }

    inline void truncateFrom(uint32_t step) {
        auto& s = state();
        s.macro.erase(s.macro.begin() + static_cast<std::ptrdiff_t>(lowerBoundIdx(step)), s.macro.end());
    }

    inline void clearGrace() {
        auto& s = state();
        s.graceMask = 0;
        s.isFrozen = false;
    }

    // Приводит удержания в игре к target. Вызывать ТОЛЬКО из processCommands.
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
            }
            for (int b = 0; b < 4; ++b) {
                uint8_t bit = bitOf(p1, b);

                if ((target & ~cur) & bit) {
                    gl->handleButton(true, b, p1);
                }
            }
        }

        s.gameHeld = target;
        s.injecting = wasInjecting;
    }

    // ---------- окно возрождения (grace) ----------

    inline void beginGrace() {
        auto& s = state();

        // ВАЖНО: берём реально зажатые клавиши, а не хвост макроса —
        // именно их нужно сохранить, чтобы дэш-орб не «отпустился».
        s.graceMask = s.finger;
        s.isFrozen = true;
        s.freezeEndTime = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(RESPAWN_FREEZE_MS);
    }

    // Разрешение окна возрождения. ВЫЗЫВАЕТСЯ ИЗ processCommands (до оригинала) —
    // это и есть фикс дэш-орба на записи.
    inline void resolveGrace(GJBaseGameLayer* gl) {
        auto& s = state();

        uint8_t mask = s.graceMask;
        s.graceMask = 0;
        s.isFrozen = false;
        s.syncHeld = false;

        // 1. Полностью синхронизируем игру с нашим представлением.
        applyHeldMask(gl, 0);

        // 2. Пишем в макрос только ИЗМЕНЕНИЯ относительно состояния чекпоинта.
        uint8_t base = heldNow();
        for (int p = 0; p < 2; ++p) {
            bool p1 = (p == 0);
            for (int b = 0; b < 4; ++b) {
                uint8_t bit = bitOf(p1, b);
                bool nowHeld  = (mask & bit) != 0;
                bool baseHeld = (base & bit) != 0;

                if (nowHeld != baseHeld) {
                    push(s.step, static_cast<uint8_t>(b), nowHeld, p1);
                }
            }
        }

        // 3. Возвращаем в игру всё, что физически зажато.
        applyHeldMask(gl, mask);
    }

    // Завершение записи без доступа к слою (выход/смена режима/levelComplete).
    inline void finishGraceOffline() {
        auto& s = state();

        uint8_t mask = s.graceMask;
        s.graceMask = 0;
        s.isFrozen = false;

        uint8_t base = heldNow();
        for (int p = 0; p < 2; ++p) {
            bool p1 = (p == 0);
            for (int b = 0; b < 4; ++b) {
                uint8_t bit = bitOf(p1, b);
                bool nowHeld  = (mask & bit) != 0;
                bool baseHeld = (base & bit) != 0;

                if (nowHeld != baseHeld) {
                    push(s.step, static_cast<uint8_t>(b), nowHeld, p1);
                }
            }
        }
    }

    inline void seek(uint32_t step) {
        auto& s = state();

        s.step = step;
        s.playIdx = lowerBoundIdx(step);
        s.syncHeld = true;
        s.tpsAccumulator = 0.0;
        // gameHeld НЕ трогаем: loadFromCheckpoint может сам дёрнуть handleButton,
        // а рассинхрон лечится applyHeldMask(0) в resolveGrace/inject.
    }

    inline void resetToStart() {
        auto& s = state();

        seek(0);
        s.gameHeld = 0;
        clearGrace();

        if (s.mode == Mode::Record) {
            truncateFrom(0);
        }
    }

    inline void inject(GJBaseGameLayer* gl) {
        auto& s = state();

        // Маска ДО событий текущего шага.
        uint8_t target = 0;
        if (s.playIdx > 0 && s.playIdx <= s.macro.size()) {
            target = s.macro[s.playIdx - 1].held;
        }

        size_t n = s.macro.size();
        while (s.playIdx < n && s.macro[s.playIdx].step <= s.step) {
            Input const& e = s.macro[s.playIdx++];

            uint8_t m = bitOf(e.p1, e.button);
            if (!m) continue;

            if (e.down) target |= m;
            else        target &= static_cast<uint8_t>(~m);
        }

        s.syncHeld = false;
        applyHeldMask(gl, target);
    }

    inline void save();   // fwd (используется в stopRecording)

    inline void stopRecording() {
        auto& s = state();
        if (s.mode != Mode::Record) return;

        finishGraceOffline();
        s.mode = Mode::Off;
        clearGrace();
        save();
    }

    // ---------- пути и имена ----------

    inline std::filesystem::path macrosRootDir() {
        return Mod::get()->getSaveDir() / "macros";
    }

    inline std::filesystem::path macroDir() {
        return macrosRootDir() / state().levelKey;
    }

    inline std::filesystem::path macroPath(std::string const& name) {
        return macroDir() / (name + ".gdbm");
    }

    inline std::string sanitizeName(std::string const& raw) {
        std::string out;
        for (char c : raw) {
            unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == ' ') {
                out += c;
            }
        }
        while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        if (out.size() > 64) out.resize(64);
        return out;
    }

    inline bool isValidName(std::string const& n) {
        return !n.empty() && n.size() <= 64 && n.find("..") == std::string::npos;
    }

    // ---------- сохранение ----------

    inline std::mutex& saveMutex() {
        static std::mutex m;
        return m;
    }

    inline std::atomic_bool& saveBusy() {
        static std::atomic_bool b{false};
        return b;
    }

    // v3: [GDBM][version=3][count][tps] + count * (step:u32, button:u8, flags:u8)
    inline std::vector<char> serializeMacro(std::vector<Input> const& macro, uint32_t tpsForSave) {
        uint32_t version = 3;
        uint32_t count = static_cast<uint32_t>(macro.size());

        std::vector<char> buf(16 + static_cast<size_t>(count) * 6);

        std::memcpy(buf.data(), "GDBM", 4);
        std::memcpy(buf.data() + 4, &version, 4);
        std::memcpy(buf.data() + 8, &count, 4);
        std::memcpy(buf.data() + 12, &tpsForSave, 4);

        char* p = buf.data() + 16;
        for (auto const& e : macro) {
            uint8_t flags = static_cast<uint8_t>((e.down ? 1 : 0) | (e.p1 ? 2 : 0));
            std::memcpy(p, &e.step, 4);
            p[4] = static_cast<char>(e.button);
            p[5] = static_cast<char>(flags);
            p += 6;
        }
        return buf;
    }

    inline bool writeMacroFile(std::filesystem::path const& path, std::vector<char> const& buf) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            log::error("GDBot: can't create macro dir: {}", ec.message());
            return false;
        }

        auto tmpPath = path.string() + ".tmp";

        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            log::error("GDBot: can't open temp macro file: {}", tmpPath);
            return false;
        }

        f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        f.flush();
        f.close();

        std::filesystem::rename(tmpPath, path, ec);
        if (ec) {
            log::error("GDBot: rename failed: {}", ec.message());
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
        return true;
    }

    inline void saveSlotSync(std::filesystem::path const& path,
                             std::vector<Input> const& macroCopy,
                             uint32_t tps) {
        auto buf = serializeMacro(macroCopy, tps);

        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(saveMutex());
            ok = writeMacroFile(path, buf);
        }

        if (ok) {
            log::info("GDBot: saved {} inputs to '{}'", macroCopy.size(), path.filename().string());
        }
        saveBusy().store(false);
    }

    inline void save() {
        auto& s = state();
        if (s.levelKey.empty() || !isValidName(s.macroName)) return;

        if (saveBusy().exchange(true)) {
            log::warn("GDBot: previous save still in progress, skipping");
            saveBusy().store(false);
            return;
        }

        auto path = macroPath(s.macroName);
        auto macroCopy = s.macro;
        uint32_t tps = s.tps;

        std::thread([path, tps, macroCopy = std::move(macroCopy)]() mutable {
            saveSlotSync(path, macroCopy, tps);
        }).detach();
    }

    inline bool saveAs(std::string const& name) {
        if (!isValidName(name)) return false;
        state().macroName = name;
        save();
        return true;
    }

    // ---------- загрузка ----------

    struct MacroFileHeader {
        bool ok = false;
        uint32_t version = 0;
        uint32_t count = 0;
        uint32_t tps = 240;
        size_t headerSize = 16;
    };

    inline MacroFileHeader readHeader(std::ifstream& f, uint64_t size) {
        MacroFileHeader h;
        if (size < 16) return h;

        char hdr[16];
        f.read(hdr, 16);
        if (f.gcount() != 16) return h;
        if (std::memcmp(hdr, "GDBM", 4) != 0) return h;

        std::memcpy(&h.version, hdr + 4, 4);
        std::memcpy(&h.count, hdr + 8, 4);

        if (h.version == 1) {
            h.headerSize = 12;
            h.tps = 240;
        } else if (h.version == 2 || h.version == 3) {
            std::memcpy(&h.tps, hdr + 12, 4);
            h.headerSize = 16;
        } else {
            return h;
        }

        if (h.tps < State::kMinTps || h.tps > State::kMaxTps) h.tps = 240;

        if (static_cast<uint64_t>(h.count) * 6 + h.headerSize != size) return h;
        if (h.count > MAX_MACRO_INPUTS) return h;

        h.ok = true;
        return h;
    }

    inline size_t loadFromFile(std::filesystem::path const& path,
                               std::vector<Input>& out,
                               uint32_t& outTps) {
        out.clear();
        outTps = 240;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
            return 0;
        }

        uint64_t size = std::filesystem::file_size(path, ec);
        if (ec || size < 16) return 0;

        std::ifstream f(path, std::ios::binary);
        if (!f) return 0;

        auto h = readHeader(f, size);
        if (!h.ok) {
            log::warn("GDBot: bad macro file '{}'", path.filename().string());
            return 0;
        }

        outTps = h.tps;

        std::vector<char> body(static_cast<size_t>(h.count) * 6);
        if (h.count > 0) {
            f.seekg(static_cast<std::streamoff>(h.headerSize));
            f.read(body.data(), static_cast<std::streamsize>(body.size()));
            if (!f) return 0;
        }

        out.reserve(h.count);

        uint32_t lastStep = 0;
        uint8_t held = 0;
        char const* p = body.data();

        for (uint32_t i = 0; i < h.count; ++i, p += 6) {
            uint32_t step = 0;
            std::memcpy(&step, p, 4);

            if (step < lastStep) {
                log::warn("GDBot: macro not sorted at record {}, stopping", i);
                break;
            }
            lastStep = step;

            uint8_t button = static_cast<uint8_t>(p[4]);
            if (button > 3) continue;

            uint8_t flags = static_cast<uint8_t>(p[5]);
            bool down = (flags & 1) != 0;
            bool p1 = (flags & 2) != 0;

            uint8_t m = bitOf(p1, button);
            if (!m) continue;
            if (((held & m) != 0) == down) continue;   // дедупликация

            held = down ? static_cast<uint8_t>(held | m)
                        : static_cast<uint8_t>(held & ~m);

            out.push_back({step, button, down, p1, held});
        }

        return out.size();
    }

    inline void load(std::string const& name) {
        auto& s = state();

        s.macro.clear();
        s.playIdx = 0;
        s.macroName = isValidName(name) ? name : std::string("default");

        if (s.levelKey.empty()) return;

        uint32_t fileTps = 240;
        std::vector<Input> loaded;
        size_t n = loadFromFile(macroPath(s.macroName), loaded, fileTps);

        s.macro = std::move(loaded);

        // Макрос привязан к номерам ШАГОВ, поэтому TPS файла — это TPS,
        // на котором его имеет смысл играть. (Менять потом можно — шкала та же.)
        setTps(fileTps);

        // ВАЖНО: индекс считаем уже по новому макросу.
        s.playIdx = lowerBoundIdx(s.step);
        s.syncHeld = true;

        log::info("GDBot: loaded {} inputs (slot '{}', tps {})", n, s.macroName, fileTps);
    }

    inline void load() {
        load(state().macroName);
    }

    struct MacroSlotInfo {
        std::string name;
        size_t count = 0;
        uint32_t tps = 240;
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

            MacroSlotInfo info;
            info.name = path.stem().string();
            if (!isValidName(info.name)) continue;

            std::ifstream f(path, std::ios::binary);
            if (f) {
                uint64_t size = std::filesystem::file_size(path, ec);
                if (!ec) {
                    auto h = readHeader(f, size);
                    if (h.ok) {
                        info.count = h.count;
                        info.tps = h.tps;
                    }
                }
            }

            result.push_back(std::move(info));
        }

        std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
            return a.name < b.name;
        });

        return result;
    }

    inline bool slotExists(std::string const& name) {
        std::error_code ec;
        return std::filesystem::exists(macroPath(name), ec);
    }

    inline bool deleteSlot(std::string const& name) {
        if (!isValidName(name)) return false;

        // Не удаляем файл, который прямо сейчас пишется.
        std::lock_guard<std::mutex> lock(saveMutex());
        std::error_code ec;
        std::filesystem::remove(macroPath(name).string() + ".tmp", ec);
        return std::filesystem::remove(macroPath(name), ec);
    }

    inline void migrateLegacyMacro() {
        auto& s = state();
        if (s.levelKey.empty()) return;

        std::error_code ec;
        auto legacyPath = macrosRootDir() / (s.levelKey + ".gdbm");
        if (!std::filesystem::exists(legacyPath, ec)) return;

        std::filesystem::create_directories(macroDir(), ec);
        auto newPath = macroPath("default");

        if (!std::filesystem::exists(newPath, ec)) {
            std::filesystem::rename(legacyPath, newPath, ec);
            if (!ec) log::info("GDBot: migrated legacy macro to slot 'default'");
        }
    }

    inline std::string makeLevelKey(GJGameLevel* level) {
        if (!level) return "unknown";

        // m_levelID в биндингах может быть std::optional<int> — берём безопасно.
        int id = 0;
        {
            auto raw = level->m_levelID;
            std::optional<int> opt{raw};
            if (opt.has_value()) id = opt.value();
        }

        if (id > 0) return std::to_string(id);

        std::string name = level->m_levelName;
        for (auto& c : name) {
            unsigned char u = static_cast<unsigned char>(c);
            if (!std::isalnum(u) && c != '-' && c != '_') c = '_';
        }
        if (name.empty()) name = "unnamed";
        if (name.size() > 48) name.resize(48);

        return "local_" + name;
    }

    // Вызывается ОДИН раз на вход в уровень.
    inline void onEnterLevel(GJGameLevel* level) {
        auto& s = state();

        s.mode = Mode::Off;
        s.step = 0;
        s.playIdx = 0;
        s.macro.clear();

        s.syncHeld = false;
        s.injecting = false;

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
        resetMeasure();

        s.levelKey = makeLevelKey(level);
        s.macroName = "default";

        migrateLegacyMacro();
        load();
    }

    // Смена режима из меню паузы: состояние сбрасывается, слот НЕ перезагружается.
    inline void prepareModeSwitch() {
        auto& s = state();

        s.step = 0;
        s.playIdx = 0;
        s.finger = 0;
        s.gameHeld = 0;
        s.tpsAccumulator = 0.0;
        s.cpFrames.clear();
        clearGrace();
        resetMeasure();
    }
}

// =====================================================================
//  Физический шаг и ввод
// =====================================================================

class $modify(BotBaseLayer, GJBaseGameLayer) {

    void update(float dt) {
        auto& s = bot::state();

        if (!bot::isPlayLayer(this) || !s.isFrozen || s.mode != bot::Mode::Record) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // Пауза после возрождения: не даём окну закрыться, пока игра на паузе.
        if (m_isPaused) {
            s.freezeEndTime = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(bot::RESPAWN_FREEZE_MS);
            GJBaseGameLayer::update(dt);
            return;
        }

        // Время вышло — сам grace НЕ закрываем: это делает хук processCommands
        // ПЕРЕД оригиналом, чтобы зажатие попало в систему ввода игрока
        // в правильной точке кадра (именно так работает воспроизведение).
        if (std::chrono::steady_clock::now() >= s.freezeEndTime) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // Заморозка: кадр не обновляем вообще.
    }

    // Квантование дельты движка на сетку 1/tps + аккумулятор остатка.
    float getModifiedDelta(float dt) {
        auto& s = bot::state();

        if (!bot::isPlayLayer(this)) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }

        // Наследуем все штатные модификаторы (timeWarp, ускорения и т.п.).
        float base = GJBaseGameLayer::getModifiedDelta(dt);
        if (!(base > 0.f)) {
            s.tpsAccumulator = 0.0;
            return base;
        }

        double spt = 1.0 / static_cast<double>(std::max<uint32_t>(s.tps, bot::State::kMinTps));

        double total = static_cast<double>(base) + s.tpsAccumulator;

        double maxTotal = spt * bot::MAX_STEPS_PER_FRAME;
        if (total > maxTotal) total = maxTotal;

        double steps = std::floor(total / spt + 1e-9);
        double newDelta = steps * spt;

        s.tpsAccumulator = total - newDelta;

        return static_cast<float>(newDelta);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = bot::state();
        bool inPlay = bot::isPlayLayer(this);

        if (inPlay) {
            bool const frozenOver =
                s.isFrozen && std::chrono::steady_clock::now() >= s.freezeEndTime;

            // --- 1. Окно возрождения разрешается ИМЕННО ЗДЕСЬ ---
            if (s.mode == bot::Mode::Record && (frozenOver || (!s.isFrozen && s.graceMask != 0))) {
                bot::resolveGrace(this);
            } else if (s.mode != bot::Mode::Record && (s.isFrozen || s.graceMask != 0)) {
                bot::clearGrace();
            }

            // --- 2. Воспроизведение ---
            if (s.mode == bot::Mode::Play) {
                bool hasMore = s.playIdx < s.macro.size();
                if (s.syncHeld || (hasMore && s.macro[s.playIdx].step <= s.step)) {
                    bot::inject(this);
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (inPlay) {
            ++s.step;
            bot::countStep();
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = bot::state();

        bool const gameButton = (button >= 0 && button <= 3);
        bool const inPlay = bot::isPlayLayer(this);

        if (!s.injecting && inPlay && gameButton) {

            if (s.mode == bot::Mode::Play) {
                // Пока макрос не кончился — живой ввод игнорируется.
                // После конца макроса управление возвращается игроку.
                if (s.playIdx < s.macro.size() || s.syncHeld) {
                    return;
                }
                // иначе падаем вниз и обрабатываем как обычный ввод
            }

            if (s.mode == bot::Mode::Record) {
                // Служебные вызовы движка во время reset/загрузки чекпоинта —
                // не реальный ввод игрока.
                bool const isEngineReset = s.inResetLevel || (s.cpLoadDepth > 0);

                if (!isEngineReset) {
                    uint8_t m = bot::bitOf(isPlayer1, button);

                    if (down) s.finger |= m;
                    else      s.finger &= static_cast<uint8_t>(~m);

                    if (s.isFrozen) {
                        // Внутри 150-мс окна ввод НЕ пишем и в игру НЕ передаём:
                        // итоговое состояние будет выставлено в resolveGrace().
                        return;
                    }

                    bot::push(s.step, static_cast<uint8_t>(button), down, isPlayer1);
                }
            }
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!s.injecting && inPlay && gameButton) {
            uint8_t m = bot::bitOf(isPlayer1, button);
            if (m) {
                if (down) s.gameHeld |= m;
                else      s.gameHeld &= static_cast<uint8_t>(~m);
            }
        }
    }
};

// =====================================================================
//  Уровень: вход, сбросы, чекпоинты, смерть
// =====================================================================

class $modify(BotPlayLayer, PlayLayer) {

    void onEnter() {
        PlayLayer::onEnter();

        auto& s = bot::state();

        // onEnter() срабатывает и при возврате из паузы — в таком случае
        // уровень НЕ переинициализируем (иначе слот/макрос сбрасывались).
        if (s.enteredLayer == static_cast<void*>(this)) {
            return;
        }
        s.enteredLayer = static_cast<void*>(this);

        bot::onEnterLevel(m_level);
    }

    void onExit() {
        auto& s = bot::state();

        if (s.enteredLayer == static_cast<void*>(this)) {
            s.enteredLayer = nullptr;

            if (s.mode == bot::Mode::Record) {
                bot::finishGraceOffline();
                bot::save();
            }
            s.mode = bot::Mode::Off;
            bot::clearGrace();
            s.cpFrames.clear();
        }

        PlayLayer::onExit();
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
            if (s.mode == bot::Mode::Record && !m_isPracticeMode) {
                // Обычный режим: смерть = конец попытки. Фиксируем и сохраняем.
                bot::stopRecording();
            }

            bot::resetToStart();
            s.cpFrames.clear();

            if (s.mode == bot::Mode::Record) {
                bot::beginGrace();
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
        // Намеренно БЕЗ grace: это явный рестарт, а не возрождение.
    }

    void storeCheckpoint(CheckpointObject* cp) {
        PlayLayer::storeCheckpoint(cp);
        if (cp) bot::state().cpFrames[cp] = bot::state().step;
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
                // Чекпоинт неизвестен — начинаем с того, что реально зажато.
                bot::beginGrace();
            }
            return;
        }

        bot::seek(it->second);

        if (s.mode == bot::Mode::Record) {
            bot::truncateFrom(it->second);
            bot::beginGrace();     // маска = РЕАЛЬНО зажатые клавиши (s.finger)
        }
    }

    void levelComplete() {
        if (bot::state().mode == bot::Mode::Record) {
            bot::finishGraceOffline();
            bot::save();
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        auto& s = bot::state();

        if (s.mode == bot::Mode::Record) {
            bot::finishGraceOffline();
            bot::save();
        }

        s.mode = bot::Mode::Off;
        bot::clearGrace();
        s.enteredLayer = nullptr;

        PlayLayer::onQuit();
    }
};

// =====================================================================
//  UI: попапы
//
//  Всё построено на geode::Popup<> — он сам решает:
//    * модальность (затемнение + блокировка тачей нижележащих слоёв),
//    * приоритеты (вложенные попапы открываются поверх),
//    * Escape / кнопка «назад» (onClose),
//    * безопасное удаление из собственного колбэка.
//  Никаких ручных CCLayerColor с ccTouchBegan и «магических» touch priority.
// =====================================================================

// ---------------- Ввод числа (TPS / имя слота) ----------------

struct PromptParams {
    std::string title;
    std::string placeholder;
    std::string initial;
    std::function<void(std::string)> onOk;
    // Быстрые пресеты (для TPS). Пусто — пресетов нет.
    std::vector<uint32_t> presets;
};

class BotPromptPopup : public Popup<PromptParams> {
protected:
    geode::TextInput* m_input = nullptr;

    bool setup(PromptParams params) override {
        this->setTitle(params.title);

        m_input = geode::TextInput::create(m_size.width - 40.f, params.placeholder);
        if (!m_input) return false;

        m_input->setString(params.initial);
        m_input->setPosition(m_size.width / 2.f, m_size.height - 40.f);
        m_mainLayer->addChild(m_input);

        // Только цифры, максимум 6 символов (100000).
        m_input->setInputFilter(geode::InputFilter::Numbers);
        if (auto* node = m_input->getInputNode()) {
            node->setAllowedChars("0123456789");
            node->setMaxCharCount(6);
        }

        float const rowY = 24.f;

        if (!params.presets.empty()) {
            auto menu = CCMenu::create();
            menu->setPosition(0.f, 0.f);
            m_mainLayer->addChild(menu);

            int n = static_cast<int>(params.presets.size());
            float const stepX = (m_size.width - 40.f) / static_cast<float>(n);

            for (int i = 0; i < n; ++i) {
                auto txt = std::to_string(params.presets[static_cast<size_t>(i)]);
                auto spr = ButtonSprite::create(txt.c_str(), 0x28, 0, .55f);
                spr->setScale(.75f);

                auto btn = CCMenuItemSpriteExtra::create(
                    spr, this, menu_selector(BotPromptPopup::onPreset)
                );
                btn->setTag(static_cast<int>(params.presets[static_cast<size_t>(i)]));
                btn->setPosition(20.f + stepX * (static_cast<float>(i) + .5f), m_size.height - 78.f);
                menu->addChild(btn);
            }
        }

        auto menu = CCMenu::create();
        menu->setPosition(0.f, 0.f);
        m_mainLayer->addChild(menu);

        auto okSpr = ButtonSprite::create("OK", 0x48, 0, .6f);
        auto ok = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(BotPromptPopup::onOkClicked));
        ok->setPosition(m_size.width / 2.f + 34.f, rowY);
        menu->addChild(ok);

        auto cancelSpr = ButtonSprite::create("Cancel", 0x60, 0, .6f);
        auto cancel = CCMenuItemSpriteExtra::create(cancelSpr, this, menu_selector(BotPromptPopup::onCancelClicked));
        cancel->setPosition(m_size.width / 2.f - 38.f, rowY);
        menu->addChild(cancel);

        return true;
    }

    void applyPreset(int v) {
        if (!m_input) return;
        m_input->setString(std::to_string(v));
    }

public:
    void onPreset(CCObject* sender) {
        if (!sender) return;
        this->applyPreset(static_cast<CCNode*>(sender)->getTag());
    }

    void onOkClicked(CCObject*) {
        std::string value = m_input ? std::string(m_input->getString()) : std::string();
        auto cb = m_params.onOk;

        // Сначала закрываем себя (отложенно — безопасно из activate()),
        // потом уже дёргаем колбэк: так колбэк не работает с умирающим попапом.
        this->onClose(nullptr);

        if (cb) cb(value);
    }

    void onCancelClicked(CCObject*) {
        this->onClose(nullptr);
    }

    static BotPromptPopup* create(PromptParams params) {
        return BotPromptPopup::create(
            std::move(params),
            PopupOptions::from(320.f, 150.f)
        );
    }
};

// ---------------- Подтверждение (Да/Нет) ----------------

struct ConfirmParams {
    std::string title;
    std::string text;
    std::function<void()> onYes;
};

class BotConfirmPopup : public Popup<ConfirmParams> {
protected:
    bool setup(ConfirmParams params) override {
        this->setTitle(params.title);

        auto label = CCLabelBMFont::create(params.text.c_str(), "chatFont.fnt");
        label->setScale(.55f);
        label->setAlignment(kCCTextAlignmentCenter);
        label->setPosition(m_size.width / 2.f, m_size.height / 2.f + 10.f);
        label->limitLabelWidth(m_size.width - 30.f, .55f, 2.f);
        m_mainLayer->addChild(label);

        auto menu = CCMenu::create();
        menu->setPosition(0.f, 0.f);
        m_mainLayer->addChild(menu);

        auto yesSpr = ButtonSprite::create("Yes", 0x48, 0, .6f);
        auto yes = CCMenuItemSpriteExtra::create(yesSpr, this, menu_selector(BotConfirmPopup::onYesClicked));
        yes->setPosition(m_size.width / 2.f + 34.f, 22.f);
        menu->addChild(yes);

        auto noSpr = ButtonSprite::create("No", 0x60, 0, .6f);
        auto no = CCMenuItemSpriteExtra::create(noSpr, this, menu_selector(BotConfirmPopup::onNoClicked));
        no->setPosition(m_size.width / 2.f - 30.f, 22.f);
        menu->addChild(no);

        return true;
    }

public:
    void onYesClicked(CCObject*) {
        auto cb = m_params.onYes;
        this->onClose(nullptr);
        if (cb) cb();
    }

    void onNoClicked(CCObject*) {
        this->onClose(nullptr);
    }

    static BotConfirmPopup* create(ConfirmParams params) {
        return BotConfirmPopup::create(
            std::move(params),
            PopupOptions::from(300.f, 140.f)
        );
    }
};

// ---------------- Менеджер макросов ----------------

class BotMacroPopup : public Popup<> {
protected:
    static constexpr int kPerPage = 8;
    static constexpr int kRowTag = 424242;

    CCMenu* m_listMenu = nullptr;
    CCMenu* m_bottomMenu = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;

    std::vector<bot::MacroSlotInfo> m_slots;
    int m_page = 0;
    bool m_refreshScheduled = false;

    bool setup() override {
        this->setTitle("GD Bot Macros");

        m_statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_statusLabel->setScale(.45f);
        m_statusLabel->setPosition(m_size.width / 2.f, m_size.height - 42.f);
        m_mainLayer->addChild(m_statusLabel);

        // Кнопки страниц — сверху.
        auto pageMenu = CCMenu::create();
        pageMenu->setPosition(0.f, 0.f);
        m_mainLayer->addChild(pageMenu);

        auto prevSpr = ButtonSprite::create("<", 0x20, 0, .6f);
        auto prev = CCMenuItemSpriteExtra::create(prevSpr, this, menu_selector(BotMacroPopup::onPrevPage));
        prev->setPosition(40.f, m_size.height - 62.f);
        pageMenu->addChild(prev);

        auto nextSpr = ButtonSprite::create(">", 0x20, 0, .6f);
        auto next = CCMenuItemSpriteExtra::create(nextSpr, this, menu_selector(BotMacroPopup::onNextPage));
        next->setPosition(m_size.width - 40.f, m_size.height - 62.f);
        pageMenu->addChild(next);

        m_pageLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_pageLabel->setScale(.5f);
        m_pageLabel->setPosition(m_size.width / 2.f, m_size.height - 62.f);
        m_mainLayer->addChild(m_pageLabel);

        // Список слотов.
        m_listMenu = CCMenu::create();
        m_listMenu->setPosition(0.f, 0.f);
        m_mainLayer->addChild(m_listMenu);

        // Низ: Save as / Close.
        m_bottomMenu = CCMenu::create();
        m_bottomMenu->setPosition(0.f, 0.f);
        m_mainLayer->addChild(m_bottomMenu);

        auto saveSpr = ButtonSprite::create("Save current as...", 0x100, 0, .55f);
        saveSpr->setScale(.75f);
        auto saveBtn = CCMenuItemSpriteExtra::create(saveSpr, this, menu_selector(BotMacroPopup::onSaveAs));
        saveBtn->setPosition(m_size.width / 2.f - 82.f, 22.f);
        m_bottomMenu->addChild(saveBtn);

        auto closeSpr = ButtonSprite::create("Close", 0x60, 0, .6f);
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSpr, this, menu_selector(BotMacroPopup::onCloseClicked));
        closeBtn->setPosition(m_size.width / 2.f + 78.f, 22.f);
        m_bottomMenu->addChild(closeBtn);

        this->rebuildList();
        this->startRefreshTimer();

        return true;
    }

    // Обновление списка ПО ФАКТУ завершения фоновой записи (а не через 0.3 с наугад).
    void startRefreshTimer() {
        if (m_refreshScheduled) return;
        m_refreshScheduled = true;
        this->pollSave();
    }

    void pollSave() {
        this->scheduleOnce(
            [this](float) {
                if (!m_refreshScheduled) return;

                if (bot::saveBusy().load()) {
                    this->updateStatus();
                    this->pollSave();          // ещё пишем — опрашиваем дальше
                    return;
                }

                m_refreshScheduled = false;
                this->rebuildList();
            },
            0.1f
        );
    }

    void rebuildList() {
        m_slots = bot::listSlots();

        int const totalPages = std::max(1, (static_cast<int>(m_slots.size()) + kPerPage - 1) / kPerPage);
        if (m_page >= totalPages) m_page = totalPages - 1;
        if (m_page < 0) m_page = 0;

        m_listMenu->removeAllChildren();

        // Строки-подписи живут не в меню — убираем старые по тегу.
        std::vector<CCNode*> stale;
        if (auto* children = m_mainLayer->getChildren()) {
            for (auto* node : CCArrayExt<CCNode*>(children)) {
                if (node->getTag() == kRowTag) stale.push_back(node);
            }
        }
        for (auto* n : stale) n->removeFromParent();

        auto const& s = bot::state();

        float const topY = m_size.height - 88.f;
        float const rowH = 24.f;

        int const first = m_page * kPerPage;
        int const shown = std::min(kPerPage, static_cast<int>(m_slots.size()) - first);

        for (int i = 0; i < shown; ++i) {
            auto const& slot = m_slots[static_cast<size_t>(first + i)];
            float const y = topY - static_cast<float>(i) * rowH;

            bool const isCurrent = (slot.name == s.macroName);

            auto label = CCLabelBMFont::create(
                fmt::format("{}{} ({} inp, {} tps)", isCurrent ? "> " : "   ",
                            slot.name, slot.count, slot.tps).c_str(),
                "chatFont.fnt"
            );
            label->setScale(.5f);
            label->setAnchorPoint({0.f, .5f});
            label->setTag(kRowTag);
            if (isCurrent) label->setColor({120, 255, 140});
            label->limitLabelWidth(m_size.width - 190.f, .5f, 2.f);
            label->setPosition({14.f, y});
            m_mainLayer->addChild(label);

            auto loadSpr = ButtonSprite::create("Load", 0x50, 0, .55f);
            auto loadBtn = CCMenuItemSpriteExtra::create(loadSpr, this, menu_selector(BotMacroPopup::onLoadSlot));
            loadBtn->setTag(first + i);
            loadBtn->setPosition({m_size.width - 110.f, y});
            m_listMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("Del", 0x40, 0, .55f);
            auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(BotMacroPopup::onDeleteSlot));
            delBtn->setTag(first + i);
            delBtn->setPosition({m_size.width - 44.f, y});
            m_listMenu->addChild(delBtn);
        }

        if (m_slots.empty()) {
            auto label = CCLabelBMFont::create("No saved macros for this level yet", "chatFont.fnt");
            label->setScale(.5f);
            label->setTag(kRowTag);
            label->setPosition(m_size.width / 2.f, topY - rowH);
            m_mainLayer->addChild(label);
        }

        m_pageLabel->setString(fmt::format("Page {}/{}", m_page + 1, totalPages).c_str());

        updateStatus();
    }

    void updateStatus() {
        auto const& s = bot::state();
        std::string saving = bot::saveBusy().load() ? "  [saving...]" : "";

        m_statusLabel->setString(
            fmt::format("Slot: {}   Inputs: {}   TPS: {} (measured {}){}",
                        s.macroName, s.macro.size(), s.tps, s.measuredTps, saving).c_str()
        );
    }

    std::string nameAt(CCObject* sender) const {
        if (!sender) return {};
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= static_cast<int>(m_slots.size())) return {};
        return m_slots[static_cast<size_t>(idx)].name;
    }

public:
    void onPrevPage(CCObject*) { if (m_page > 0) { --m_page; rebuildList(); } }
    void onNextPage(CCObject*) {
        int const totalPages = std::max(1, (static_cast<int>(m_slots.size()) + kPerPage - 1) / kPerPage);
        if (m_page + 1 < totalPages) { ++m_page; rebuildList(); }
    }

    void onLoadSlot(CCObject* sender) {
        std::string name = this->nameAt(sender);
        if (name.empty()) return;

        bot::load(name);

        auto* pl = PlayLayer::get();
        auto const mode = bot::state().mode;

        // Закрываем попап ДО любых действий с уровнем.
        this->onClose(nullptr);

        if (pl && (mode == bot::Mode::Play || mode == bot::Mode::Record)) {
            // Отложенно: resetLevelFromStart() НЕ должен выполняться внутри
            // activate() кнопки попапа (иначе сносим узлы, которые ещё в стеке).
            pl->scheduleOnce([pl](float) {
                if (PlayLayer::get() == pl) pl->resetLevelFromStart();
            }, 0.f);
        }
    }

    void onDeleteSlot(CCObject* sender) {
        std::string name = this->nameAt(sender);
        if (name.empty()) return;

        // Ref держит этот попап живым, пока открыт диалог подтверждения.
        Ref<BotMacroPopup> self(this);

        auto popup = BotConfirmPopup::create(ConfirmParams{
            "Delete macro",
            fmt::format("Delete '{}'?", name),
            [self, name]() {
                bot::deleteSlot(name);

                if (bot::state().macroName == name) {
                    bot::state().macroName = "default";
                }

                // Кнопка Del к этому моменту уже может быть удалена из меню —
                // пересборку списка откладываем на следующий тик.
                self->scheduleOnce([self](float) { self->rebuildList(); }, 0.f);
            }
        });

        if (popup) popup->show();
    }

    void onSaveAs(CCObject*) {
        auto popup = BotPromptPopup::create(PromptParams{
            "Save macro as",
            "name",
            bot::state().macroName,
            [self = Ref<BotMacroPopup>(this)](std::string raw) {
                self->doSaveAs(raw);
            },
            {}
        });

        if (popup) popup->show();
    }

    void doSaveAs(std::string const& raw) {
        std::string name = bot::sanitizeName(raw);

        if (!bot::isValidName(name)) {
            FLAlertLayer::create("GD Bot", "Invalid name.\nUse letters, digits, space, - _ .", "OK")->show();
            return;
        }

        if (bot::saveBusy().load()) {
            FLAlertLayer::create("GD Bot", "Previous save is still in progress.", "OK")->show();
            return;
        }

        if (bot::slotExists(name)) {
            Ref<BotMacroPopup> self(this);

            auto popup = BotConfirmPopup::create(ConfirmParams{
                "Overwrite macro",
                fmt::format("Slot '{}' already exists.\nOverwrite?", name),
                [self, name]() {
                    bot::saveAs(name);
                    self->startRefreshTimer();
                }
            });
            if (popup) popup->show();
            return;
        }

        bot::saveAs(name);
        this->startRefreshTimer();
    }

    void onCloseClicked(CCObject*) {
        this->onClose(nullptr);
    }

    void onClose(CCObject* sender) override {
        m_refreshScheduled = false;
        this->unscheduleAll();
        Popup::onClose(sender);
    }

    static BotMacroPopup* create() {
        return BotMacroPopup::create(PopupOptions::from(400.f, 320.f));
    }
};

// =====================================================================
//  Меню паузы
// =====================================================================

class $modify(BotPauseLayer, PauseLayer) {

    struct Fields {
        CCMenu* menu = nullptr;          // кнопка «Bot»
        CCMenu* panel = nullptr;         // вся панель (пересоздаётся целиком)
        CCLabelBMFont* info = nullptr;   // ребёнок PauseLayer, НЕ panel
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
            toggleSpr, this, menu_selector(BotPauseLayer::onBotToggle)
        );
        toggle->setPosition({x, topY});
        menu->addChild(toggle);

        auto panel = CCMenu::create();
        panel->setPosition({0.f, 0.f});
        this->addChild(panel);
        m_fields->panel = panel;

        m_fields->info = CCLabelBMFont::create("", "bigFont.fnt");
        m_fields->info->setScale(.3f);
        m_fields->info->setAlignment(kCCTextAlignmentCenter);
        m_fields->info->setPosition({x, topY - 26.f * 6 - 26.f});
        this->addChild(m_fields->info);

        this->rebuildBotMenu();
    }

    CCMenuItemSpriteExtra* makeBotButton(char const* text, bot::Mode mode) {
        bool selected = (bot::state().mode == mode);

        auto spr = ButtonSprite::create(
            text,
            "bigFont.fnt",
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
        auto panel = m_fields ? m_fields->panel : nullptr;
        if (!panel || !m_fields->info) return;

        auto& s = bot::state();
        auto win = CCDirector::get()->getWinSize();

        float const x = 95.f;
        float const topY = win.height * 0.62f;

        // removeAllChildren() у CCMenu безопасен, если вызывающая кнопка
        // удержана снаружи (см. rebuildKeeping) либо вызов идёт не из activate().
        panel->removeAllChildren();

        char const* names[3] = { "Record", "Off", "Play" };
        bot::Mode modes[3] = { bot::Mode::Record, bot::Mode::Off, bot::Mode::Play };

        for (int i = 0; i < 3; ++i) {
            auto btn = this->makeBotButton(names[i], modes[i]);
            btn->setPosition({x, topY - 26.f * static_cast<float>(i + 1)});
            panel->addChild(btn);
        }

        // --- Строка TPS:  [-] [+]  <число>  [Set] ---
        float const tpsRowY = topY - 26.f * 4;

        auto decSpr = ButtonSprite::create("-", "bigFont.fnt", "GJ_button_04.png", .7f);
        decSpr->setScale(.6f);
        auto decBtn = CCMenuItemSpriteExtra::create(decSpr, this, menu_selector(BotPauseLayer::onTpsDec));
        decBtn->setPosition({x - 60.f, tpsRowY});
        panel->addChild(decBtn);

        auto incSpr = ButtonSprite::create("+", "bigFont.fnt", "GJ_button_04.png", .7f);
        incSpr->setScale(.6f);
        auto incBtn = CCMenuItemSpriteExtra::create(incSpr, this, menu_selector(BotPauseLayer::onTpsInc));
        incBtn->setPosition({x - 30.f, tpsRowY});
        panel->addChild(incBtn);

        auto tpsLabel = CCLabelBMFont::create(fmt::format("{}", s.tps).c_str(), "bigFont.fnt");
        tpsLabel->setScale(.42f);
        tpsLabel->setPosition({x + 8.f, tpsRowY});
        panel->addChild(tpsLabel);

        auto setSpr = ButtonSprite::create("Set", "bigFont.fnt", "GJ_button_04.png", .6f);
        setSpr->setScale(.55f);
        auto setBtn = CCMenuItemSpriteExtra::create(setSpr, this, menu_selector(BotPauseLayer::onTpsCustom));
        setBtn->setPosition({x + 62.f, tpsRowY});
        panel->addChild(setBtn);

        // --- Менеджер макросов ---
        auto macrosSpr = ButtonSprite::create("Macros...", "bigFont.fnt", "GJ_button_04.png", .7f);
        macrosSpr->setScale(.6f);
        auto macrosBtn = CCMenuItemSpriteExtra::create(macrosSpr, this, menu_selector(BotPauseLayer::onOpenMacros));
        macrosBtn->setPosition({x, topY - 26.f * 5});
        panel->addChild(macrosBtn);

        panel->setVisible(s.panelOpen);
        m_fields->info->setVisible(s.panelOpen);

        this->refreshInfo();
    }

    // Только текст статуса — без пересборки кнопок.
    void refreshInfo() {
        if (!m_fields || !m_fields->info) return;

        auto& s = bot::state();

        m_fields->info->setString(
            fmt::format(
                "GD Bot: {}\nSlot: {} ({} inputs)\nTPS: {} (measured: {})",
                bot::modeName(s.mode),
                s.macroName,
                s.macro.size(),
                s.tps,
                s.measuredTps
            ).c_str()
        );
    }

    // Пересборка панели ИЗ колбэка её же кнопки: удерживаем sender,
    // пока выполняется его activate().
    void rebuildKeeping(CCObject* sender) {
        Ref<CCObject> keep(sender);
        this->rebuildBotMenu();
    }

    void onBotToggle(CCObject* sender) {
        bot::state().panelOpen = !bot::state().panelOpen;
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

    // РУЧНОЙ ВВОД TPS: любое целое 1..100000.
    void onTpsCustom(CCObject*) {
        // Ref не даёт слою паузы умереть, пока открыт попап.
        Ref<BotPauseLayer> self(this);

        auto popup = BotPromptPopup::create(PromptParams{
            "Custom TPS",
            "1 - 100000",
            fmt::format("{}", bot::state().tps),
            [self](std::string value) {
                auto v = bot::parseTps(value);
                if (!v) {
                    FLAlertLayer::create(
                        "GD Bot",
                        "Enter a number from 1 to 100000.",
                        "OK"
                    )->show();
                    return;
                }

                bot::setTps(*v);
                bot::resetMeasure();

                // Пересобираем панель только если слой ещё живой и в сцене.
                if (self && self->getParent()) {
                    self->rebuildBotMenu();
                }
            },
            { 60, 120, 240, 480, 1000, 2000, 4000, 10000 }
        });

        if (popup) popup->show();
    }

    void onOpenMacros(CCObject*) {
        Ref<BotPauseLayer> self(this);

        auto popup = BotMacroPopup::create();
        if (!popup) return;

        popup->setCloseCallback([self](bool) {
            if (self && self->getParent()) {
                self->refreshInfo();
            }
        });

        popup->show();
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
        auto* pl = PlayLayer::get();

        if (!pl) return;
        if (!sender) return;

        auto mode = static_cast<bot::Mode>(static_cast<CCNode*>(sender)->getTag());
        if (mode == s.mode) return;

        // Сохраняем предыдущую запись, если она шла.
        if (s.mode == bot::Mode::Record) {
            bot::finishGraceOffline();
            bot::save();
        }

        if (mode == bot::Mode::Play && s.macro.empty()) {
            FLAlertLayer::create(
                "GD Bot",
                "Macro is empty.\nRecord one first.",
                "OK"
            )->show();
            return;
        }

        s.mode = mode;
        bot::prepareModeSwitch();

        if (mode == bot::Mode::Record) {
            s.macro.clear();
            s.macro.reserve(std::max<size_t>(1u << 14, static_cast<size_t>(s.tps) * 60u));
        }

        // 1) СНАЧАЛА пересборка панели (sender ещё жив, мы внутри его activate()).
        this->rebuildKeeping(sender);

        if (mode == bot::Mode::Off) {
            return;   // паузу не закрываем — игрок сам нажмёт Resume
        }

        // 2) Рестарт уровня.
        pl->resetLevelFromStart();

        // 3) Выход из паузы — ПОСЛЕДНИМ и ОТЛОЖЕННО: PauseLayer уничтожается,
        //    а мы всё ещё находимся внутри activate() его кнопки.
        //    Именно здесь был краш в прошлой версии (onResume(nullptr) в середине колбэка).
        this->scheduleOnce([pl](float) {
            if (PlayLayer::get() == pl) {
                pl->togglePauseGame(false);
            }
        }, 0.f);
    }
};
