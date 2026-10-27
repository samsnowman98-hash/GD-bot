// =====================================================================
//  GD Bot — macro recorder / replayer для Geometry Dash 2.2081 (Geode 5.x)
//  ВЕРСИЯ 2 — исправленная сборка
// =====================================================================
//
//  НАБОР ФУНКЦИЙ ПОЛНОСТЬЮ СОХРАНЁН:
//   * три режима (Off / Record / Play) из меню паузы, кнопка «Bot» сворачивает
//     панель, состояние свёрнутости запоминается;
//   * TPS: ручной ввод 1..100000 (поле + пресеты), кнопки +/- крутят пресеты
//     60..4000, рядом показывается «measured» — реальная частота шагов;
//   * менеджер макросов: пагинация по 8, отметка текущего слота, Load/Del,
//     подтверждение удаления и перезаписи, «Save current as...», санитизация имён;
//   * слоты лежат в <save>/macros/<levelKey>/<slot>.gdbm, легаси-файлы
//     <save>/macros/<levelKey>.gdbm переносятся автоматически;
//   * формат v3 (читаются v1/v2/v3, при сохранении всё переносится в v3);
//   * запись: переходы кнопок по шагам, авто-сохранение на смерти (обычный
//     режим), на прохождении уровня и при выходе;
//   * практика: чекпоинт откатывает шаг и обрезает макрос;
//   * воспроизведение: маска удержаний инжектится ПЕРЕД processCommands,
//     после конца макроса управление возвращается игроку;
//   * окно возрождения 150 мс: физически зажатые клавиши продолжают действовать,
//     в макрос пишется только итоговое состояние (фикс дэш-орба);
//   * фоновая запись файла с атомарным rename и статусом «[saving...]».
//
// ---------------------------------------------------------------------
//  ЧТО ИЗМЕНЕНО ОТНОСИТЕЛЬНО ПРЕДЫДУЩЕЙ ВЕРСИИ
// ---------------------------------------------------------------------
//  БЛОКЕРЫ КОМПИЛЯЦИИ (проверено по bindings/2.2081 и geode-sdk v5.11.2):
//   [B1] getModifiedDelta возвращает double, а не float:
//        `double getModifiedDelta(float dt)` — сигнатура хука исправлена.
//        Заодно дельта больше никогда не бывает 0 (при steps < 1 возвращается
//        исходная base), иначе движок может получить 1/dt = inf.
//   [B2] scheduleOnce(lambda, float) в Geode 5.x НЕ существует — есть только
//        scheduleOnce(SEL_SCHEDULE, float). Все отложенные вызовы переведены на
//        свой узел BotDeferred, который живёт на сцене и тикает даже когда
//        PlayLayer стоит на паузе (старый вариант на pl->scheduleOnce просто
//        никогда бы не сработал из меню паузы).
//   [B3] PlayLayer::togglePauseGame не существует в 2.2081. Используются
//        реальные методы: PlayLayer::resumeAndRestart(bool) и PlayLayer::resume().
//   [B4] GJGameLevel::m_levelID — это geode::SeedValueRSV (античит-обёртка с
//        operator int()), а не std::optional. Прежняя «безопасная» обёртка
//        std::optional<int> компилировалась, но has_value() был всегда true,
//        то есть не защищала ни от чего. Теперь: проверка level на nullptr +
//        однократное чтение value().
//   [B5] TextInput: setCommonFilter(CommonFilter::Uint)/setFilter + setMaxCharCount
//        вместо ручного setAllowedChars; добавлен focus() после показа.
//
//  ЛОГИКА:
//   [F1] inject(): состояние ДО событий шага теперь считается из реально
//        применённых событий (playMask), а не из поля .held соседней записи.
//        Побочно исправлена потеря первого ввода, записанного на шаге 0
//        (раньше playIdx == 0 давал target = 0 и событие проглатывалось).
//   [F2] save(): больше не сбрасывает чужой флаг занятости. Очередь из одной
//        отложенной задачи + постоянный воркер вместо detached-потоков.
//   [F3] resolveGrace(): applyHeldMask(gl, 0) заменён на forceReleaseAll(gl) —
//        реальный отпуск всех кнопок независимо от того, что нам «кажется»
//        про gameHeld (после loadFromCheckpoint оно могло расходиться).
//   [F4] levelComplete(): запись теперь именно ЗАВЕРШАЕТСЯ (stopRecording),
//        а не сохраняется с продолжающимся режимом Record.
//   [F5] processCommands(): шаги и grace считаются только когда игра реально
//        идёт (PlayLayer::m_isPaused == false). Иначе measured TPS и шкала шагов
//        портились, если processCommands вызывается на паузе.
//   [F6] s.finger обновляется в любом режиме, а не только в Record.
//   [F7] onResume из паузы во время окна возрождения продлевает его на полные
//        150 мс — иначе пауза «съедала» grace и орб терялся.
//   [F8] Сериализация little-endian явно (не memcpy uint32) — формат переносим.
//
//  КРАШИ / ЖИВУЧЕСТЬ:
//   [C1] Панель в PauseLayer больше НЕ пересобирается через removeAllChildren
//        внутри activate() собственной кнопки. Виджеты создаются один раз,
//        дальше обновляются только спрайты (setSprite) и текст (setString).
//   [C2] s.enteredLayer чистится в PlayLayer::onExit — раньше висячий указатель
//        мог совпасть с адресом нового PlayLayer, и уровень не инициализировался.
//   [C3] Весь наш код в PauseLayer::onResume выполняется ДО вызова оригинала
//        (оригинал уничтожает слой).
//   [C4] Воркер записи — один постоянный поток с join(); detached-потоки,
//        переживающие выгрузку мода, убраны. stopSaveWorker() — для вызова
//        из $on_unload, если он вам нужен.
//   [C5] getModifiedDelta не возвращает 0 (см. B1).
//   [C6] Все действия, ломающие дерево узлов (закрытие попапа, рестарт уровня,
//        пересборка списка), вынесены из колбэков кнопок на следующий тик.
//
//  UI:
//   [U1] Попапы переведены на geode::Popup (наследник FLAlertLayer/CCBlockLayer):
//        модальность, force-priority тачей, keypad, кнопка закрытия и show()
//        берутся из фреймворка. Ручная схема с приоритетами -9000/-9100 удалена —
//        она не гарантировала победу над CCBlockLayer самого PauseLayer.
//   [U2] Диалог подтверждения — geode::createQuickPopup вместо своего класса.
//   [U3] Кнопки списка создаются через CCMenuItemExt::createSpriteExtra с
//        лямбдой, захватывающей имя слота: никаких setTag/индексов, которые
//        разъезжались при пересборке списка.
//
// ---------------------------------------------------------------------
//  ОГРАНИЧЕНИЕ ПО TPS (честно, как и было)
// ---------------------------------------------------------------------
//  Реальная частота физики задаётся тем, как движок дробит getModifiedDelta()
//  на свои подшаги. Мы квантуем дельту на сетку 1/tps и копим остаток в
//  аккумуляторе, поэтому число шагов processCommands() растёт вместе с tps —
//  но только пока движок согласен дробить кадр, и не больше
//  MAX_STEPS_PER_FRAME шагов за кадр. На очень высоких tps упрётесь в потолок
//  движка. В меню показывается «measured» — ориентируйтесь на него.
// =====================================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Layout.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>          // INT_MAX
#include <condition_variable>
#include <cstddef>          // size_t, std::byte
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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
        uint8_t held;   // накопленная маска после этого события (формат/отладка)
    };

    struct State {
        // Ручной ввод TPS: 1..100000 (как и было).
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
        uint8_t playMask = 0;           // маска, реально применённая к событию playIdx-1

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
        bool inExplicitRestart = false; // рестарт из меню, а не смерть

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

    /// Игра реально идёт: мы в PlayLayer и он не на паузе. [F5]
    inline bool gameRunning() {
        auto* pl = PlayLayer::get();
        return pl != nullptr && !pl->m_isPaused;
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

    // Крутит пресеты вокруг текущего значения (которое может быть ручным).
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

    /// Маска удержаний после применения событий [0, idx). [F1]
    inline uint8_t foldMaskBefore(size_t idx) {
        auto const& m = state().macro;
        uint8_t mask = 0;
        for (size_t i = 0; i < idx && i < m.size(); ++i) {
            uint8_t b = bitOf(m[i].p1, m[i].button);
            if (!b) continue;
            if (m[i].down) mask |= b;
            else           mask &= static_cast<uint8_t>(~b);
        }
        return mask;
    }

    inline void recomputePlayMask() {
        auto& s = state();
        if (s.playIdx > s.macro.size()) s.playIdx = s.macro.size();
        s.playMask = foldMaskBefore(s.playIdx);
    }

    inline void truncateFrom(uint32_t step) {
        auto& s = state();
        s.macro.erase(s.macro.begin() + static_cast<std::ptrdiff_t>(lowerBoundIdx(step)), s.macro.end());
        if (s.playIdx > s.macro.size()) s.playIdx = s.macro.size();
        recomputePlayMask();
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

    /// БЕЗУСЛОВНО отпускает все кнопки. Нужен после loadFromCheckpoint(),
    /// где s.gameHeld может расходиться с реальным состоянием игры. [F3]
    inline void forceReleaseAll(GJBaseGameLayer* gl) {
        auto& s = state();

        bool const wasInjecting = s.injecting;
        s.injecting = true;

        for (int p = 0; p < 2; ++p) {
            bool const p1 = (p == 0);
            for (int b = 0; b < 4; ++b) {
                gl->handleButton(false, b, p1);
            }
        }

        s.injecting = wasInjecting;
        s.gameHeld = 0;
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

    /// Пишет в макрос разницу между маской и состоянием чекпоинта.
    inline void pushMaskDelta(uint8_t mask) {
        auto& s = state();

        uint8_t base = heldNow();
        for (int p = 0; p < 2; ++p) {
            bool const p1 = (p == 0);
            for (int b = 0; b < 4; ++b) {
                uint8_t bit = bitOf(p1, b);
                bool const nowHeld  = (mask & bit) != 0;
                bool const baseHeld = (base & bit) != 0;

                if (nowHeld != baseHeld) {
                    push(s.step, static_cast<uint8_t>(b), nowHeld, p1);
                }
            }
        }
    }

    // Разрешение окна возрождения. ВЫЗЫВАЕТСЯ ИЗ processCommands (до оригинала) —
    // это и есть фикс дэш-орба на записи.
    inline void resolveGrace(GJBaseGameLayer* gl) {
        auto& s = state();

        uint8_t const mask = s.graceMask;
        s.graceMask = 0;
        s.isFrozen = false;
        s.syncHeld = false;

        // 1. Честно отпускаем ВСЁ (не полагаясь на s.gameHeld). [F3]
        forceReleaseAll(gl);

        // 2. Пишем в макрос только изменения относительно состояния чекпоинта.
        pushMaskDelta(mask);

        // 3. Возвращаем в игру всё, что физически зажато.
        applyHeldMask(gl, mask);

        // 4. Маска воспроизведения могла измениться вместе с хвостом записи.
        recomputePlayMask();
    }

    // Завершение записи без доступа к слою (выход/смена режима/levelComplete).
    inline void finishGraceOffline() {
        auto& s = state();

        uint8_t const mask = s.graceMask;
        s.graceMask = 0;
        s.isFrozen = false;

        pushMaskDelta(mask);
        recomputePlayMask();
    }

    inline void seek(uint32_t step) {
        auto& s = state();

        s.step = step;
        s.playIdx = lowerBoundIdx(step);
        s.syncHeld = true;
        s.tpsAccumulator = 0.0;
        recomputePlayMask();
        // gameHeld НЕ трогаем: loadFromCheckpoint может сам дёрнуть handleButton,
        // а рассинхрон лечится forceReleaseAll() в resolveGrace.
    }

    inline void resetToStart() {
        auto& s = state();

        seek(0);
        s.gameHeld = 0;
        s.playMask = 0;
        clearGrace();

        if (s.mode == Mode::Record) {
            truncateFrom(0);
            recomputePlayMask();
        }
    }

    inline void inject(GJBaseGameLayer* gl) {
        auto& s = state();

        // Маска ДО событий текущего шага — из реально применённых событий. [F1]
        uint8_t target = s.playMask;

        size_t const n = s.macro.size();
        while (s.playIdx < n && s.macro[s.playIdx].step <= s.step) {
            Input const& e = s.macro[s.playIdx++];

            uint8_t m = bitOf(e.p1, e.button);
            if (!m) continue;

            if (e.down) target |= m;
            else        target &= static_cast<uint8_t>(~m);
        }

        s.playMask = target;
        s.syncHeld = false;
        applyHeldMask(gl, target);
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
}

// =====================================================================
//  Фоновое сохранение: очередь из одной задачи + постоянный воркер
//  [F2][C4] Раньше save() сбрасывал чужой флаг занятости и плодил
//  detached-потоки, которые переживали выгрузку мода.
// =====================================================================

namespace bot {

    struct SaveJob {
        std::filesystem::path path;
        std::vector<Input> macro;
        uint32_t tps = 240;
        std::string slot;
    };

    inline std::atomic_bool& saveBusy() {
        static std::atomic_bool b{false};
        return b;
    }

    // Сериализация не может идти параллельно с другой сериализацией
    // (обе читают s.macro в главном потоке) — но главное, она не должна
    // пересекаться с записью того же слота.
    inline std::mutex& jobMutex() {
        static std::mutex m;
        return m;
    }

    inline std::condition_variable& jobCv() {
        static std::condition_variable c;
        return c;
    }

    inline std::optional<SaveJob>& pendingJob() {
        static std::optional<SaveJob> j;
        return j;
    }

    inline bool& workerStopping() {
        static bool b = false;
        return b;
    }

    inline std::mutex& fileMutex() {
        static std::mutex m;
        return m;
    }

    inline std::thread& saveWorker() {
        static std::thread t;
        return t;
    }

    // ---------- формат файла ----------
    // v3: [GDBM][version=3][count][tps] + count * (step:u32 LE, button:u8, flags:u8)

    inline void putU32LE(char* p, uint32_t v) {                       // [F8]
        p[0] = static_cast<char>(v & 0xFF);
        p[1] = static_cast<char>((v >> 8) & 0xFF);
        p[2] = static_cast<char>((v >> 16) & 0xFF);
        p[3] = static_cast<char>((v >> 24) & 0xFF);
    }

    inline uint32_t getU32LE(char const* p) {
        return static_cast<uint32_t>(static_cast<uint8_t>(p[0]))
             | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8)
             | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16)
             | (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
    }

    inline std::vector<char> serializeMacro(std::vector<Input> const& macro, uint32_t tpsForSave) {
        uint32_t const count = static_cast<uint32_t>(macro.size());

        std::vector<char> buf(16 + static_cast<size_t>(count) * 6);

        std::memcpy(buf.data(), "GDBM", 4);
        putU32LE(buf.data() + 4, 3u);
        putU32LE(buf.data() + 8, count);
        putU32LE(buf.data() + 12, tpsForSave);

        char* p = buf.data() + 16;
        for (auto const& e : macro) {
            uint8_t const flags = static_cast<uint8_t>((e.down ? 1 : 0) | (e.p1 ? 2 : 0));
            putU32LE(p, e.step);
            p[4] = static_cast<char>(e.button);
            p[5] = static_cast<char>(flags);
            p += 6;
        }
        return buf;
    }

    inline std::string tmpPathFor(std::filesystem::path const& path) {
        return path.string() + "." +
               std::to_string(std::hash<std::string>{}(path.filename().string())) + ".tmp";
    }

    inline bool writeMacroFile(std::filesystem::path const& path, std::vector<char> const& buf) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            log::error("GDBot: can't create macro dir: {}", ec.message());
            return false;
        }

        // Уникальное имя временного файла: два разных слота не должны
        // писать в один и тот же .tmp (иначе гонка на rename).
        auto const tmpPath = tmpPathFor(path);

        {
            std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
            if (!f) {
                log::error("GDBot: can't open temp macro file: {}", tmpPath);
                return false;
            }
            f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            f.flush();
            if (!f) {
                log::error("GDBot: write failed: {}", tmpPath);
                return false;
            }
        }

        std::filesystem::rename(tmpPath, path, ec);
        if (ec) {
            log::error("GDBot: rename failed: {}", ec.message());
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
        return true;
    }

    /// Записать то, что лежит в очереди, ПРЯМО СЕЙЧАС (в текущем потоке).
    /// Используется на выходе из уровня, чтобы файл точно лёг на диск.
    inline void drainSaveQueue() {
        SaveJob job;
        {
            std::lock_guard<std::mutex> lk(jobMutex());
            if (!pendingJob()) return;
            job = std::move(*pendingJob());
            pendingJob().reset();
        }

        auto buf = serializeMacro(job.macro, job.tps);
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(fileMutex());
            ok = writeMacroFile(job.path, buf);
        }
        if (ok) {
            log::info("GDBot: saved {} inputs to '{}'", job.macro.size(), job.slot);
        }
        saveBusy().store(false);
    }

    inline void workerLoop() {
        for (;;) {
            SaveJob job;
            {
                std::unique_lock<std::mutex> lk(jobMutex());
                jobCv().wait(lk, [] { return pendingJob().has_value() || workerStopping(); });

                if (!pendingJob()) {
                    if (workerStopping()) return;
                    continue;
                }

                job = std::move(*pendingJob());
                pendingJob().reset();
            }

            auto buf = serializeMacro(job.macro, job.tps);   // тяжёлое — вне jobMutex
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(fileMutex());
                ok = writeMacroFile(job.path, buf);
            }
            if (ok) {
                log::info("GDBot: saved {} inputs to '{}'", job.macro.size(), job.slot);
            }

            // Флаг занятости снимаем только если в очереди ничего не осталось,
            // иначе UI на мгновение показал бы «сохранено» посреди серии записей.
            {
                std::lock_guard<std::mutex> lk(jobMutex());
                if (!pendingJob()) saveBusy().store(false);
            }
        }
    }

    inline void ensureWorker() {
        if (!saveWorker().joinable()) {
            workerStopping() = false;
            saveWorker() = std::thread(workerLoop);
        }
    }

    inline void save() {
        auto& s = state();
        if (s.levelKey.empty() || !isValidName(s.macroName)) return;

        {
            std::lock_guard<std::mutex> lk(jobMutex());
            // Перезаписываем ещё не стартовавшую задачу — это НЕ потеря данных:
            // в macroCopy лежит более свежий снимок того же слота.
            pendingJob() = SaveJob{ macroPath(s.macroName), s.macro, s.tps, s.macroName };
            saveBusy().store(true);
            ensureWorker();
        }
        jobCv().notify_one();
    }

    /// Синхронное сохранение (для onQuit/levelComplete): очередь дренируется
    /// в главном потоке, файл гарантированно на диске до возврата.
    inline void saveSync() {
        auto& s = state();
        if (s.levelKey.empty() || !isValidName(s.macroName)) return;

        save();

        std::unique_lock<std::mutex> lk(fileMutex());   // ждём завершения текущей записи
        lk.unlock();
        drainSaveQueue();
    }

    /// Остановка воркера. Зовите из $on_unload, если выгружаете мод на ходу.
    inline void stopSaveWorker() {
        {
            std::lock_guard<std::mutex> lk(jobMutex());
            pendingJob().reset();
            workerStopping() = true;
        }
        jobCv().notify_all();

        if (saveWorker().joinable()) {
            // Даём воркеру дописать текущий файл, но не держим главный поток вечно.
            for (int i = 0; i < 40 && saveBusy().load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            saveWorker().join();
        }
    }

    inline void stopRecording() {
        auto& s = state();
        if (s.mode != Mode::Record) return;

        finishGraceOffline();
        s.mode = Mode::Off;
        clearGrace();
        save();
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

        h.version = getU32LE(hdr + 4);
        h.count   = getU32LE(hdr + 8);

        if (h.version == 1) {
            h.headerSize = 12;
            h.tps = 240;
        } else if (h.version == 2 || h.version == 3) {
            h.tps = getU32LE(hdr + 12);
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

        uint64_t const size = std::filesystem::file_size(path, ec);
        if (ec || size < 16) return 0;

        std::ifstream f(path, std::ios::binary);
        if (!f) return 0;

        auto const h = readHeader(f, size);
        if (!h.ok) {
            log::warn("GDBot: bad macro file '{}'", path.filename().string());
            return 0;
        }

        outTps = h.tps;

        std::vector<char> body(static_cast<size_t>(h.count) * 6);
        if (h.count > 0) {
            f.clear();
            f.seekg(static_cast<std::streamoff>(h.headerSize));
            f.read(body.data(), static_cast<std::streamsize>(body.size()));
            if (!f) {
                log::warn("GDBot: truncated macro file '{}'", path.filename().string());
                return 0;
            }
        }

        out.reserve(h.count);

        uint32_t lastStep = 0;
        uint8_t held = 0;
        char const* p = body.data();

        for (uint32_t i = 0; i < h.count; ++i, p += 6) {
            uint32_t const step = getU32LE(p);

            if (step < lastStep) {
                log::warn("GDBot: macro not sorted at record {}, stopping", i);
                break;
            }
            lastStep = step;

            uint8_t const button = static_cast<uint8_t>(p[4]);
            if (button > 3) continue;

            uint8_t const flags = static_cast<uint8_t>(p[5]);
            bool const down = (flags & 1) != 0;
            bool const p1   = (flags & 2) != 0;

            uint8_t const m = bitOf(p1, button);
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
        s.playMask = 0;
        s.macroName = isValidName(name) ? name : std::string("default");

        if (s.levelKey.empty()) return;

        uint32_t fileTps = 240;
        std::vector<Input> loaded;
        size_t const n = loadFromFile(macroPath(s.macroName), loaded, fileTps);

        s.macro = std::move(loaded);

        // Макрос привязан к номерам ШАГОВ, поэтому TPS файла — это TPS,
        // на котором его имеет смысл играть. (Менять потом можно — шкала та же.)
        setTps(fileTps);

        // ВАЖНО: индекс и маска считаются уже по новому макросу.
        s.playIdx = lowerBoundIdx(s.step);
        recomputePlayMask();
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
        auto const dir = macroDir();
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

            uint64_t const size = std::filesystem::file_size(path, ec);
            if (ec) continue;

            std::ifstream f(path, std::ios::binary);
            if (f) {
                auto const h = readHeader(f, size);
                if (h.ok) {
                    info.count = h.count;
                    info.tps = h.tps;
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
        std::lock_guard<std::mutex> lk(fileMutex());

        std::error_code ec;
        std::filesystem::remove(tmpPathFor(macroPath(name)), ec);
        return std::filesystem::remove(macroPath(name), ec);
    }

    inline void migrateLegacyMacro() {
        auto& s = state();
        if (s.levelKey.empty()) return;

        std::error_code ec;
        auto const legacyPath = macrosRootDir() / (s.levelKey + ".gdbm");
        if (!std::filesystem::exists(legacyPath, ec)) return;

        std::filesystem::create_directories(macroDir(), ec);
        auto const newPath = macroPath("default");

        if (!std::filesystem::exists(newPath, ec)) {
            std::filesystem::rename(legacyPath, newPath, ec);
            if (!ec) log::info("GDBot: migrated legacy macro to slot 'default'");
        }
    }

    inline std::string makeLevelKey(GJGameLevel* level) {
        if (!level) return "unknown";                       // [B4]

        // m_levelID — geode::SeedValueRSV: у него есть int value() и operator int().
        // Никакого std::optional там нет; value() не бросает. Читаем ОДИН раз,
        // потому что value() мутирует внутреннее состояние античит-обёртки.
        int const id = level->m_levelID.value();
        if (id > 0) return std::to_string(id);

        std::string name = level->m_levelName;              // gd::string -> std::string
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
        s.playMask = 0;
        s.macro.clear();

        s.syncHeld = false;
        s.injecting = false;

        s.inResetLevel = false;
        s.cpLoadDepth = 0;
        s.cpLoadedInReset = false;
        s.inExplicitRestart = false;

        s.panelOpen = false;
        s.finger = 0;
        s.gameHeld = 0;

        s.isFrozen = false;
        s.freezeEndTime = {};

        s.tpsAccumulator = 0.0;
        s.cpFrames.clear();
        clearGrace();
        resetMeasure();

        s.levelKey = makeLevelKey(level);                   // [B4]: level может быть null
        s.macroName = "default";

        migrateLegacyMacro();
        load();

        log::info("GDBot: level '{}' (slot '{}')", s.levelKey, s.macroName);
    }

    // Смена режима из меню паузы: состояние сбрасывается, слот НЕ перезагружается.
    inline void prepareModeSwitch() {
        auto& s = state();

        s.step = 0;
        s.playIdx = 0;
        s.playMask = 0;
        s.finger = 0;
        s.gameHeld = 0;
        s.tpsAccumulator = 0.0;
        s.cpFrames.clear();
        s.syncHeld = true;
        clearGrace();
        resetMeasure();
    }
}

// =====================================================================
//  ДИАГНОСТИКА (выключена по умолчанию)
//
//  Главный непроверенный допуск всей модели: что processCommands()
//  вызывается НА КАЖДЫЙ физический подшаг, а не один раз на кадр.
//  Поставьте kBotDiagnostics = true, пересоберите, зайдите на любой уровень
//  с tps = 1000 и посмотрите в лог Geode строки вида:
//
//    GDBot[diag] 1.0s: update=144 pc=144 step=144 engineStep=240 tps=1000 measured=144
//
//  * pc/s == update/s           -> шаги = кадры; шкалу надо менять
//                                  (или брать m_currentStep из движка);
//  * pc/s ~ 240 при любом tps   -> подшаги фиксированные, tps влияет
//                                  только на длительность кадра;
//  * pc/s растёт вместе с tps   -> модель верна;
//  * engineStep (m_currentStep) -> собственный счётчик физических шагов
//                                  движка, которым пользуются нативные
//                                  реплеи 2.2. Если он совпадает с step,
//                                  можно просто перейти на него.
// =====================================================================

namespace bot {

    inline constexpr bool kBotDiagnostics = false;

    inline uint32_t& diagUpdateCount() { static uint32_t v = 0; return v; }
    inline uint32_t& diagPcCount()     { static uint32_t v = 0; return v; }

    /// Зовётся из processCommands (не чаще раза за шаг). Печатает раз в секунду.
    inline void diagnosticsTick() {
        if (!kBotDiagnostics) return;

        static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

        ++diagPcCount();

        auto const now = std::chrono::steady_clock::now();
        double const el = std::chrono::duration<double>(now - t0).count();
        if (el < 1.0) return;

        int engineStep = -1;
        if (auto* gl = static_cast<GJBaseGameLayer*>(PlayLayer::get())) {
            engineStep = gl->m_currentStep;
        }

        log::info(
            "GDBot[diag] {:.1f}s: update={} pc={} step={} engineStep={} tps={} measured={}",
            el, diagUpdateCount(), diagPcCount(), state().step,
            engineStep, state().tps, state().measuredTps
        );

        diagUpdateCount() = 0;
        diagPcCount() = 0;
        t0 = now;
    }
}


// =====================================================================
//  Отложенные вызовы
//
//  [B2] scheduleOnce(лямбда, float) в Geode 5.x не существует.
//  [C6] Плюс: узел, на котором висит scheduleOnce, во время паузы НЕ тикает,
//       поэтому планировать «рестарт уровня» на PlayLayer из меню паузы
//       бессмысленно. Deferred живёт на СЦЕНЕ и тикает всегда.
// =====================================================================

class BotDeferred : public cocos2d::CCNode {
protected:
    std::map<int, std::function<void()>> m_pending;
    int m_nextId = 1;

    bool init() override {
        if (!CCNode::init()) return false;
        this->setVisible(false);
        this->schedule(schedule_selector(BotDeferred::onTick), 0.f);
        return true;
    }

    void onTick(float) {
        if (m_pending.empty()) return;

        // Переносим пачку: колбэк имеет право класть новые задачи,
        // а итерировать живой map в таком случае нельзя.
        auto batch = std::move(m_pending);
        m_pending.clear();

        for (auto& entry : batch) {
            if (entry.second) entry.second();
        }
    }

public:
    void onEnter() override {
        CCNode::onEnter();
        // После removeFromParent(cleanup) планировщик узла чистится — взводим снова.
        this->schedule(schedule_selector(BotDeferred::onTick), 0.f);
    }

    static BotDeferred* get() {
        static BotDeferred* inst = nullptr;

        if (!inst) {
            inst = new BotDeferred();
            if (!inst->init()) {
                delete inst;
                inst = nullptr;
                return nullptr;
            }
            inst->retain();          // живёт всю сессию, не зависит от сцены
        }

        // Переезжаем вместе со сценой: после смены сцены у узла нет родителя.
        if (!inst->getParent()) {
            if (auto* scene = CCDirector::get()->getRunningScene()) {
                scene->addChild(inst, INT_MAX);
            }
        }
        return inst;
    }

    /// Выполнить fn на следующем тике. Возвращает id для отмены.
    int post(std::function<void()> fn) {
        // Если узел сейчас не в дереве (смена сцены прямо в этот момент),
        // планировщик не тикает — выполняем сразу, чтобы задача не потерялась.
        if (!this->getParent()) {
            if (fn) fn();
            return -1;
        }
        int const id = m_nextId++;
        m_pending.emplace(id, std::move(fn));
        return id;
    }

    void cancel(int id) {
        m_pending.erase(id);
    }
};

namespace bot {

    inline void alert(std::string const& text) {
        FLAlertLayer::create("GD Bot", text, "OK")->show();
    }

    /// Отложить на следующий тик. Никогда не падает, даже если сцены нет.
    inline void later(std::function<void()> fn) {
        if (auto* d = BotDeferred::get()) {
            d->post(std::move(fn));
        } else if (fn) {
            fn();      // крайний случай: выполняем сразу
        }
    }

    /// Помечает, что сейчас последует ЯВНЫЙ рестарт уровня из меню
    /// (PlayLayer::resumeAndRestart(true)). Нужен, чтобы хук resetLevel()
    /// не принял его за смерть и не перезаписал слот пустым макросом.
    inline void beginExplicitRestart() {
        state().inExplicitRestart = true;
    }

    /// Выход из паузы + рестарт с начала.
    /// Это замена несуществующему в 2.2081 togglePauseGame(false). [B3]
    /// ВАЖНО: вызывать ПОСЛЕДНИМ в колбэке кнопки — внутри resume()
    /// PauseLayer уничтожается.
    inline void restartFromMenu(PlayLayer* pl) {
        if (!pl) return;
        beginExplicitRestart();
        pl->resumeAndRestart(true);
        state().inExplicitRestart = false;
    }
}

// =====================================================================
//  UI: попапы
//
//  [U1] База — geode::Popup (наследник FLAlertLayer -> CCBlockLayer ->
//  CCLayerColor). Он сам:
//    * затемняет экран и глотает тачи (force priority 2, targeted delegate -500),
//      то есть корректно работает ПОВЕРХ PauseLayer, который тоже CCBlockLayer;
//    * даёт m_mainLayer (размера попапа, центр в центре окна) и m_buttonMenu;
//    * добавляет кнопку закрытия в левый верхний угол и обрабатывает Esc;
//    * закрывается через виртуальный onClose(CCObject*).
//  Позиционируем детей идиоматично — addChildAtPosition(node, Anchor, offset),
//  как это делает сам Geode.
// =====================================================================

namespace botui {
    inline constexpr float kMacroW = 400.f;
    inline constexpr float kMacroH = 320.f;
    inline constexpr float kPromptW = 340.f;
}

// ---------------- Ввод числа (TPS) / строки (имя слота) ----------------

struct PromptParams {
    std::string title;
    std::string placeholder;
    std::string initial;
    bool digitsOnly = false;
    std::function<void(std::string)> onOk;
    std::vector<uint32_t> presets;   // пусто — пресетов нет
};

class BotPromptPopup : public geode::Popup {
protected:
    geode::TextInput* m_input = nullptr;
    PromptParams m_params;

    bool init(PromptParams params) {
        m_params = std::move(params);

        bool const hasPresets = !m_params.presets.empty();
        float const h = hasPresets ? 170.f : 140.f;

        if (!geode::Popup::init(botui::kPromptW, h, "GJ_square01.png")) {
            return false;
        }

        this->setTitle(m_params.title.c_str(), "bigFont.fnt", .6f, 20.f);

        float const w = m_size.width;

        m_input = geode::TextInput::create(w - 50.f, m_params.placeholder);
        if (!m_input) return false;

        if (m_params.digitsOnly) {
            m_input->setCommonFilter(geode::CommonFilter::Uint);
            m_input->setMaxCharCount(6);
        } else {
            m_input->setFilter(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-."
            );
            m_input->setMaxCharCount(64);
        }
        m_input->setString(gd::string(m_params.initial));
        m_mainLayer->addChildAtPosition(m_input, Anchor::Top, ccp(0.f, -52.f));
        m_input->focus();          // [B5] клавиатура/каретка сразу активны

        if (hasPresets) {
            auto presetMenu = CCMenu::create();
            presetMenu->setContentSize(CCSize{w - 30.f, 28.f});
            presetMenu->setAnchorPoint({.5f, .5f});
            presetMenu->ignoreAnchorPointForPosition(false);
            // AutoScale не используем: он предполагает «родной» масштаб детей 1.0,
            // а у ButtonSprite он свой. Even разложит пресеты равномерно.
            presetMenu->setLayout(
                RowLayout::create()
                    ->setAxisAlignment(AxisAlignment::Even)
                    ->setCrossAxisAlignment(AxisAlignment::Center)
                    ->setGap(2.f)
            );

            for (uint32_t v : m_params.presets) {
                auto spr = ButtonSprite::create(
                    std::to_string(v).c_str(), "bigFont.fnt", "GJ_button_04.png", .4f
                );
                spr->setScale(.4f);

                auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, v](auto*) {
                    if (m_input) m_input->setString(gd::string(std::to_string(v)));
                });
                presetMenu->addChild(btn);
            }

            m_mainLayer->addChildAtPosition(presetMenu, Anchor::Top, ccp(0.f, -88.f));
        }

        auto okSpr = ButtonSprite::create("OK", "bigFont.fnt", "GJ_button_01.png", .7f);
        okSpr->setScale(.6f);
        auto okBtn = CCMenuItemSpriteExtra::create(
            okSpr, this, menu_selector(BotPromptPopup::onOkClicked)
        );
        m_buttonMenu->addChildAtPosition(okBtn, Anchor::Bottom, ccp(45.f, 22.f));

        auto cancelSpr = ButtonSprite::create("Cancel", "bigFont.fnt", "GJ_button_06.png", .7f);
        cancelSpr->setScale(.6f);
        auto cancelBtn = CCMenuItemSpriteExtra::create(
            cancelSpr, this, menu_selector(BotPromptPopup::onCancelClicked)
        );
        m_buttonMenu->addChildAtPosition(cancelBtn, Anchor::Bottom, ccp(-45.f, 22.f));

        return true;
    }

    /// Закрываем попап и только ПОТОМ зовём колбэк: колбэк может открыть
    /// другой попап или пересобрать UI, и делать это из-под нашего activate()
    /// не стоит.
    void finish(bool ok) {
        std::string value;
        if (m_input) value = std::string(m_input->getString());

        auto cb = std::move(m_params.onOk);
        m_params.onOk = nullptr;

        this->onClose(nullptr);

        if (ok && cb) cb(value);
    }

public:
    void onOkClicked(CCObject*)     { this->finish(true);  }
    void onCancelClicked(CCObject*) { this->finish(false); }

    // Крестик и Esc -> отмена (колбэк не вызываем).
    void onClose(CCObject* sender) override {
        m_params.onOk = nullptr;
        geode::Popup::onClose(sender);
    }

    static BotPromptPopup* create(PromptParams params) {
        auto ret = new BotPromptPopup();

        if (ret->init(std::move(params))) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }
};

// ---------------- Менеджер макросов ----------------

class BotMacroPopup : public geode::Popup {
protected:
    static constexpr int kPerPage = 8;
    static constexpr int kRowTag = 424242;

    CCLabelBMFont* m_statusLabel = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenu* m_pageMenu = nullptr;
    CCMenu* m_listMenu = nullptr;

    std::vector<bot::MacroSlotInfo> m_slots;
    int m_page = 0;
    bool m_polling = false;

public:
    // Вызывается при закрытии попапа (чтобы меню паузы обновило статус).
    std::function<void()> m_onClosed = nullptr;

protected:
    // override: совпадает по сигнатуре с виртуальным CCLayer::init().
    // Никакой другой init() у geode::Popup не вызывается автоматически,
    // а наш create() зовёт его явно — так что переопределение безопасно.
    bool init() override {
        if (!geode::Popup::init(botui::kMacroW, botui::kMacroH, "GJ_square01.png")) {
            return false;
        }

        this->setTitle("GD Bot Macros", "bigFont.fnt", .6f, 20.f);

        float const w = m_size.width;

        m_statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_statusLabel->setScale(.45f);
        m_mainLayer->addChildAtPosition(m_statusLabel, Anchor::Top, ccp(0.f, -44.f));

        // --- постраничная навигация ---
        m_pageMenu = CCMenu::create();
        m_pageMenu->setContentSize(CCSize{w - 20.f, 30.f});
        m_pageMenu->setAnchorPoint({.5f, .5f});
        m_pageMenu->ignoreAnchorPointForPosition(false);
        m_pageMenu->setLayout(
            RowLayout::create()
                ->setAxisAlignment(AxisAlignment::Between)
                ->setCrossAxisAlignment(AxisAlignment::Center)
        );

        auto prevSpr = ButtonSprite::create("<", "bigFont.fnt", "GJ_button_04.png", 1.f);
        prevSpr->setScale(.6f);
        m_pageMenu->addChild(CCMenuItemSpriteExtra::create(
            prevSpr, this, menu_selector(BotMacroPopup::onPrevPage)));

        m_pageLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_pageLabel->setScale(.5f);
        m_pageMenu->addChild(m_pageLabel);

        auto nextSpr = ButtonSprite::create(">", "bigFont.fnt", "GJ_button_04.png", 1.f);
        nextSpr->setScale(.6f);
        m_pageMenu->addChild(CCMenuItemSpriteExtra::create(
            nextSpr, this, menu_selector(BotMacroPopup::onNextPage)));

        m_mainLayer->addChildAtPosition(m_pageMenu, Anchor::Top, ccp(0.f, -68.f));

        // --- список слотов ---
        // Контейнер с «нулевым» размером в точке Top|(-92): дальше все строки
        // позиционируются от его левого верхнего угла вниз, как в старой версии.
        m_listMenu = CCMenu::create();
        m_listMenu->setContentSize(CCSize{w - 20.f, 0.f});
        m_listMenu->setAnchorPoint({.5f, 1.f});
        m_listMenu->ignoreAnchorPointForPosition(false);
        m_mainLayer->addChildAtPosition(m_listMenu, Anchor::Top, ccp(0.f, -92.f));

        // --- низ: Save as / Close ---
        auto saveSpr = ButtonSprite::create("Save current as...", "bigFont.fnt", "GJ_button_04.png", .6f);
        saveSpr->setScale(.6f);
        auto saveBtn = CCMenuItemSpriteExtra::create(
            saveSpr, this, menu_selector(BotMacroPopup::onSaveAs));
        m_buttonMenu->addChildAtPosition(saveBtn, Anchor::Bottom, ccp(-74.f, 22.f));

        auto closeSpr = ButtonSprite::create("Close", "bigFont.fnt", "GJ_button_06.png", .7f);
        closeSpr->setScale(.6f);
        auto closeBtn = CCMenuItemSpriteExtra::create(
            closeSpr, this, menu_selector(BotMacroPopup::onCloseClicked));
        m_buttonMenu->addChildAtPosition(closeBtn, Anchor::Bottom, ccp(78.f, 22.f));

        this->rebuildList();

        return true;
    }

    // Обновление списка ПО ФАКТУ завершения фоновой записи (а не через 0.3 с наугад).
    void startRefreshTimer() {
        if (m_polling) return;
        m_polling = true;
        this->pollSave();
    }

    void pollSave() {
        Ref<BotMacroPopup> self(this);
        bot::later([self] {
            if (!self->m_polling) return;

            if (bot::saveBusy().load()) {
                self->updateStatus();
                self->pollSave();          // ещё пишем — опрашиваем дальше
                return;
            }

            self->m_polling = false;
            self->rebuildList();
        });
    }

    void clearRows() {
        m_listMenu->removeAllChildren();

        // Подписи строк живут прямо в m_mainLayer — убираем старые по тегу.
        std::vector<CCNode*> stale;
        if (auto* children = m_mainLayer->getChildren()) {
            for (auto* node : CCArrayExt<CCNode*>(children)) {
                if (node->getTag() == kRowTag) stale.push_back(node);
            }
        }
        for (auto* n : stale) n->removeFromParent();
    }

    void rebuildList() {
        m_slots = bot::listSlots();

        int const totalPages = std::max(1, (static_cast<int>(m_slots.size()) + kPerPage - 1) / kPerPage);
        if (m_page >= totalPages) m_page = totalPages - 1;
        if (m_page < 0) m_page = 0;

        this->clearRows();

        auto const& s = bot::state();

        float const w = m_size.width;
        float const rowH = 26.f;

        int const first = m_page * kPerPage;
        int const shown = std::min(kPerPage, static_cast<int>(m_slots.size()) - first);

        for (int i = 0; i < shown; ++i) {
            auto const& slot = m_slots[static_cast<size_t>(first + i)];
            float const y = -static_cast<float>(i) * rowH;

            bool const isCurrent = (slot.name == s.macroName);

            auto label = CCLabelBMFont::create(
                fmt::format("{}{} ({} inp, {} tps)", isCurrent ? "> " : "   ",
                            slot.name, slot.count, slot.tps).c_str(),
                "chatFont.fnt"
            );
            label->setScale(.5f);
            if (isCurrent) label->setColor({120, 255, 140});
            label->setTag(kRowTag);
            m_mainLayer->addChildAtPosition(
                label, Anchor::Top, ccp(-w / 2.f + 24.f, -92.f + y), ccp(0.f, .5f), false
            );

            // [U3] Имя слота захватывается в лямбду — никаких тегов/индексов,
            // которые разъезжаются после пересборки списка.
            auto loadSpr = ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_01.png", 1.f);
            loadSpr->setScale(.5f);
            auto loadBtn = CCMenuItemExt::createSpriteExtra(loadSpr, [this, slot](auto*) {
                this->onLoadSlot(slot.name);
            });
            loadBtn->setPosition(ccp(w / 2.f - 112.f, y - rowH / 2.f + 4.f));
            m_listMenu->addChild(loadBtn);

            auto delSpr = ButtonSprite::create("Del", "bigFont.fnt", "GJ_button_06.png", 1.f);
            delSpr->setScale(.5f);
            auto delBtn = CCMenuItemExt::createSpriteExtra(delSpr, [this, slot](auto*) {
                this->onDeleteSlot(slot.name);
            });
            delBtn->setPosition(ccp(w / 2.f - 46.f, y - rowH / 2.f + 4.f));
            m_listMenu->addChild(delBtn);
        }

        if (m_slots.empty()) {
            auto label = CCLabelBMFont::create("No saved macros for this level yet", "chatFont.fnt");
            label->setScale(.5f);
            label->setTag(kRowTag);
            m_mainLayer->addChildAtPosition(label, Anchor::Top, ccp(0.f, -92.f - rowH));
        }

        m_pageLabel->setString(fmt::format("Page {}/{}", m_page + 1, totalPages).c_str());
        m_pageMenu->updateLayout();

        this->updateStatus();
    }

    void updateStatus() {
        auto const& s = bot::state();
        std::string const saving = bot::saveBusy().load() ? "  [saving...]" : "";

        m_statusLabel->setString(
            fmt::format("Slot: {}   Inputs: {}   TPS: {} (measured {}){}",
                        s.macroName, s.macro.size(), s.tps, s.measuredTps, saving).c_str()
        );
    }

public:
    void onPrevPage(CCObject*) {
        if (m_page > 0) { --m_page; this->rebuildList(); }
    }

    void onNextPage(CCObject*) {
        int const totalPages = std::max(1, (static_cast<int>(m_slots.size()) + kPerPage - 1) / kPerPage);
        if (m_page + 1 < totalPages) { ++m_page; this->rebuildList(); }
    }

    void onLoadSlot(std::string const& name) {
        if (name.empty()) return;

        bot::load(name);

        auto const mode = bot::state().mode;

        Ref<BotMacroPopup> self(this);

        // [C6] Закрытие попапа и рестарт уровня — НЕ из-под activate() кнопки.
        bot::later([self, mode] {
            self->onClose(nullptr);

            if (mode == bot::Mode::Play || mode == bot::Mode::Record) {
                // Уровень может уже быть другим — берём заново.
                bot::restartFromMenu(PlayLayer::get());   // [B3]
            }
        });
    }

    void onDeleteSlot(std::string const& name) {
        if (name.empty()) return;

        // Ref держит этот попап живым, пока открыт диалог подтверждения.
        Ref<BotMacroPopup> self(this);

        // [U2] Подтверждение — штатный быстрый попап Geode.
        geode::createQuickPopup(
            "Delete macro",
            fmt::format("Delete '{}'?", name),
            "No", "Yes",
            [self, name](FLAlertLayer*, bool btn2) {
                if (!btn2) return;

                bot::deleteSlot(name);

                if (bot::state().macroName == name) {
                    bot::state().macroName = "default";
                }

                // Кнопка Del к этому моменту уже может быть удалена из меню —
                // пересборку списка откладываем на следующий тик.
                bot::later([self] { self->rebuildList(); });
            }
        );
    }

    void onSaveAs(CCObject*) {
        Ref<BotMacroPopup> self(this);

        auto popup = BotPromptPopup::create(PromptParams{
            "Save macro as",
            "name",
            bot::state().macroName,
            false,
            [self](std::string raw) { self->doSaveAs(raw); },
            {}
        });

        if (popup) popup->show();
    }

    void doSaveAs(std::string const& raw) {
        std::string const name = bot::sanitizeName(raw);

        if (!bot::isValidName(name)) {
            bot::alert("Invalid name.\nUse letters, digits, space, - _ .");
            return;
        }

        if (bot::saveBusy().load()) {
            bot::alert("Previous save is still in progress.");
            return;
        }

        if (bot::slotExists(name)) {
            Ref<BotMacroPopup> self(this);

            geode::createQuickPopup(
                "Overwrite macro",
                fmt::format("Slot '{}' already exists.\nOverwrite?", name),
                "No", "Yes",
                [self, name](FLAlertLayer*, bool btn2) {
                    if (!btn2) return;
                    bot::saveAs(name);
                    self->startRefreshTimer();
                }
            );
            return;
        }

        bot::saveAs(name);
        this->startRefreshTimer();
    }

    void onCloseClicked(CCObject*) {
        this->onClose(nullptr);
    }

    // [C6] Снимаем опрос до вызова базового onClose, чтобы отложенный колбэк
    // не обратился к попапу, которого уже нет в дереве.
    void onClose(CCObject* sender) override {
        m_polling = false;

        auto cb = std::move(m_onClosed);
        m_onClosed = nullptr;

        geode::Popup::onClose(sender);

        if (cb) cb();
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
};

// =====================================================================
//  Физический шаг и ввод
// =====================================================================

class $modify(BotBaseLayer, GJBaseGameLayer) {

    void update(float dt) {
        auto& s = bot::state();

        if (bot::isPlayLayer(this)) {
            ++bot::diagUpdateCount();
        }

        if (!bot::isPlayLayer(this) || !s.isFrozen || s.mode != bot::Mode::Record) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // [F5] На паузе окно возрождения не «тикает»: иначе пауза съедала
        // все 150 мс и после Resume орб уже не активировался.
        if (!bot::gameRunning()) {
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
    // [B1] В 2.2081 сигнатура: double getModifiedDelta(float dt).
    double getModifiedDelta(float dt) {
        auto& s = bot::state();

        if (!bot::isPlayLayer(this)) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }

        // Наследуем все штатные модификаторы (timeWarp, ускорения и т.п.).
        double const base = GJBaseGameLayer::getModifiedDelta(dt);
        if (!(base > 0.0)) {
            s.tpsAccumulator = 0.0;
            return base;
        }

        double const spt = 1.0 / static_cast<double>(std::max<uint32_t>(s.tps, bot::State::kMinTps));
        double total = base + s.tpsAccumulator;

        double steps = std::floor(total / spt + 1e-9);

        // [C5] Никогда не возвращаем 0: ноль-дельта может дать 1/dt = inf
        // в недрах движка. Копим остаток и отдаём исходную дельту.
        if (steps < 1.0) {
            s.tpsAccumulator = total;
            return base;
        }

        if (steps > bot::MAX_STEPS_PER_FRAME) {
            steps = bot::MAX_STEPS_PER_FRAME;
        }

        double const newDelta = steps * spt;
        s.tpsAccumulator = total - newDelta;

        return newDelta;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = bot::state();

        // [F5] Шаг — это ФИЗИЧЕСКИЙ шаг. Пока игра на паузе, шкала стоит:
        // иначе measured TPS и номера шагов в макросе уезжают.
        bool const running = bot::isPlayLayer(this) && bot::gameRunning();

        if (running) {
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
                bool const hasMore = s.playIdx < s.macro.size();
                if (s.syncHeld || (hasMore && s.macro[s.playIdx].step <= s.step)) {
                    bot::inject(this);
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (running) {
            ++s.step;
            bot::countStep();
            bot::diagnosticsTick();
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = bot::state();

        bool const gameButton = (button >= 0 && button <= 3);
        bool const inPlay = bot::isPlayLayer(this);

        if (!s.injecting && inPlay && gameButton) {

            uint8_t const m = bot::bitOf(isPlayer1, button);

            // [F6] Реально зажатые клавиши отслеживаем в ЛЮБОМ режиме —
            // они нужны окну возрождения, которое стартует при смене режима.
            if (m) {
                if (down) s.finger |= m;
                else      s.finger &= static_cast<uint8_t>(~m);
            }

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
            uint8_t const m = bot::bitOf(isPlayer1, button);
            if (m) {
                if (down) s.gameHeld |= m;
                else      s.gameHeld &= static_cast<uint8_t>(~m);
            }
        }
    }
};

// =====================================================================
//  Уровень: вход/выход, сбросы, чекпоинты, смерть, прохождение
// =====================================================================

class $modify(BotPlayLayer, PlayLayer) {

    void onEnter() {
        PlayLayer::onEnter();

        auto& s = bot::state();

        // onEnter() может сработать повторно — в таком случае уровень
        // НЕ переинициализируем (иначе слот/макрос сбрасывались).
        if (s.enteredLayer == static_cast<void*>(this)) {
            return;
        }
        s.enteredLayer = static_cast<void*>(this);

        bot::onEnterLevel(m_level);
    }

    // [C2] Без этого указатель на умерший PlayLayer оставался в state, и новый
    // слой с тем же адресом (куча охотно переиспользует блоки) считался
    // «уже инициализированным» — уровень/слот не загружались.
    void onExit() {
        auto& s = bot::state();

        if (s.enteredLayer == static_cast<void*>(this)) {
            s.enteredLayer = nullptr;
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
            // inExplicitRestart: это рестарт по кнопке из меню, а НЕ смерть.
            // Без этой проверки переход в Record перезаписал бы слот пустым
            // макросом (stopRecording -> save).
            if (s.mode == bot::Mode::Record && !m_isPracticeMode && !s.inExplicitRestart) {
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

        bool const wasInResetLevel = s.inResetLevel;
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

        auto const it = s.cpFrames.find(cp);

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

    // [F4] Запись именно ЗАВЕРШАЕТСЯ: раньше mode оставался Record, и шаги
    // с вводами продолжали писаться всю энд-анимацию и LevelCompleteLayer.
    void levelComplete() {
        if (bot::state().mode == bot::Mode::Record) {
            bot::stopRecording();     // finishGraceOffline + mode=Off + save
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        auto& s = bot::state();

        if (s.mode == bot::Mode::Record) {
            bot::finishGraceOffline();
            bot::saveSync();          // синхронно: файл точно ляжет на диск
        } else {
            bot::drainSaveQueue();    // дописываем то, что уже было в очереди
        }

        s.mode = bot::Mode::Off;
        bot::clearGrace();
        s.enteredLayer = nullptr;

        PlayLayer::onQuit();
    }
};

// =====================================================================
//  Меню паузы
//
//  [C1] Панель строится ОДИН раз. Дальше обновляются только спрайты кнопок
//  (CCMenuItemSpriteExtra::setSprite — Geode-добавление) и тексты лейблов.
//  Никакого removeAllChildren() из-под activate() собственной кнопки:
//  Ref<> на sender спасал только sender, а cleanup() по соседям и по самой
//  кнопке в середине activate() — это и был источник плавающих крашей.
// =====================================================================

class $modify(BotPauseLayer, PauseLayer) {

    struct Fields {
        CCMenu* menu = nullptr;          // кнопка «Bot»
        CCMenu* panel = nullptr;         // вся панель (строится один раз)
        CCLabelBMFont* info = nullptr;   // ребёнок PauseLayer, НЕ panel

        CCMenuItemSpriteExtra* modeBtns[3] = { nullptr, nullptr, nullptr };
        CCLabelBMFont* tpsLabel = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto const win = CCDirector::get()->getWinSize();

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

        this->buildBotPanel(x, topY);
        this->refreshBotPanel();
    }

    ButtonSprite* makeModeSprite(char const* text, bool selected) {
        auto spr = ButtonSprite::create(
            text, "bigFont.fnt",
            selected ? "GJ_button_01.png" : "GJ_button_04.png",
            .7f
        );
        spr->setScale(.6f);
        return spr;
    }

    void buildBotPanel(float x, float topY) {
        auto panel = m_fields->panel;

        char const* names[3] = { "Record", "Off", "Play" };
        bot::Mode const modes[3] = { bot::Mode::Record, bot::Mode::Off, bot::Mode::Play };

        for (int i = 0; i < 3; ++i) {
            auto btn = CCMenuItemSpriteExtra::create(
                this->makeModeSprite(names[i], false),
                this, menu_selector(BotPauseLayer::onBotMode)
            );
            btn->setTag(static_cast<int>(modes[i]));
            btn->setPosition({x, topY - 26.f * static_cast<float>(i + 1)});
            panel->addChild(btn);
            m_fields->modeBtns[i] = btn;
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

        m_fields->tpsLabel = CCLabelBMFont::create("240", "bigFont.fnt");
        m_fields->tpsLabel->setScale(.42f);
        m_fields->tpsLabel->setPosition({x + 8.f, tpsRowY});
        panel->addChild(m_fields->tpsLabel);

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
    }

    // Обновление «выбранности» кнопок, числа TPS и видимости панели.
    // Дёргать можно откуда угодно, в том числе из колбэка кнопки панели.
    void refreshBotPanel() {
        if (!m_fields->panel) return;

        auto const& s = bot::state();

        m_fields->panel->setVisible(s.panelOpen);
        m_fields->info->setVisible(s.panelOpen);

        char const* names[3] = { "Record", "Off", "Play" };
        bot::Mode const modes[3] = { bot::Mode::Record, bot::Mode::Off, bot::Mode::Play };

        for (int i = 0; i < 3; ++i) {
            auto btn = m_fields->modeBtns[i];
            if (!btn) continue;
            // setSprite — Geode-добавление, сам разбирается с размером/позицией.
            btn->setSprite(this->makeModeSprite(names[i], s.mode == modes[i]));
        }

        if (m_fields->tpsLabel) {
            m_fields->tpsLabel->setString(fmt::format("{}", s.tps).c_str());
        }

        this->refreshInfo();
    }

    // Только текст статуса.
    void refreshInfo() {
        if (!m_fields->info) return;

        auto const& s = bot::state();

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

    void onBotToggle(CCObject*) {
        bot::state().panelOpen = !bot::state().panelOpen;
        this->refreshBotPanel();
    }

    void onTpsDec(CCObject*) {
        bot::cycleTps(-1);
        this->refreshBotPanel();
    }

    void onTpsInc(CCObject*) {
        bot::cycleTps(+1);
        this->refreshBotPanel();
    }

    // РУЧНОЙ ВВОД TPS: любое целое 1..100000.
    void onTpsCustom(CCObject*) {
        // Ref не даёт слою паузы умереть, пока открыт попап.
        Ref<BotPauseLayer> self(this);

        auto popup = BotPromptPopup::create(PromptParams{
            "Custom TPS",
            "1 - 100000",
            fmt::format("{}", bot::state().tps),
            true,   // digitsOnly: только цифры, до 6 символов
            [self](std::string value) {
                auto v = bot::parseTps(value);
                if (!v) {
                    bot::alert("Enter a number from 1 to 100000.");
                    return;
                }

                bot::setTps(*v);
                bot::resetMeasure();

                // Обновляем панель только если слой ещё живой и в сцене.
                if (self && self->getParent()) {
                    self->refreshBotPanel();
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

        popup->m_onClosed = [self]() {
            if (self && self->getParent()) {
                self->refreshInfo();
            }
        };

        popup->show();
    }

    // [C3] Весь наш код — ДО вызова оригинала: PauseLayer::onResume
    // снимает слой с дерева и чистит его, после этого трогать this нельзя.
    void onResume(CCObject* sender) {
        auto& s = bot::state();

        bot::resetMeasure();

        if (s.mode == bot::Mode::Play) {
            s.syncHeld = true;
            bot::recomputePlayMask();
        }

        // [F7] Пауза не должна «съедать» окно возрождения.
        if (s.mode == bot::Mode::Record && s.isFrozen) {
            s.freezeEndTime = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(bot::RESPAWN_FREEZE_MS);
        }

        PauseLayer::onResume(sender);
    }

    void onBotMode(CCObject* sender) {
        auto& s = bot::state();

        if (!sender) return;

        auto* pl = PlayLayer::get();
        if (!pl) return;

        auto const mode = static_cast<bot::Mode>(static_cast<CCNode*>(sender)->getTag());
        if (mode != bot::Mode::Off && mode != bot::Mode::Record && mode != bot::Mode::Play) {
            return;
        }
        if (mode == s.mode) return;

        // Сохраняем предыдущую запись, если она шла.
        if (s.mode == bot::Mode::Record) {
            bot::finishGraceOffline();
            bot::save();
        }

        if (mode == bot::Mode::Play && s.macro.empty()) {
            bot::alert("Macro is empty.\nRecord one first.");
            return;
        }

        s.mode = mode;
        bot::prepareModeSwitch();

        if (mode == bot::Mode::Record) {
            s.macro.clear();
            s.macro.reserve(std::max<size_t>(1u << 14, static_cast<size_t>(s.tps) * 60u));
            bot::recomputePlayMask();
        }

        // 1) Панель обновляем, пока sender жив и мы внутри его activate().
        //    Ничего не удаляем — только спрайты и текст. [C1]
        this->refreshBotPanel();

        if (mode == bot::Mode::Off) {
            return;   // паузу не закрываем — игрок сам нажмёт Resume
        }

        // 2) ПОСЛЕДНИМ: выход из паузы + рестарт одним вызовом. [B3]
        //    Внутри resume PauseLayer уничтожается, поэтому после этой строки
        //    не трогаем ни this, ни m_fields, ни sender — сразу выходим.
        bot::restartFromMenu(pl);
    }
};

// =====================================================================
//  Если вы выгружаете мод на ходу — раскомментируйте и остановите воркер:
//
//  void $on_unload(dynamic) {
//      bot::stopSaveWorker();
//  }
// =====================================================================
