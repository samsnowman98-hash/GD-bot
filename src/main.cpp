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
//  4) Попапы вынесены в общий базовый класс BotPopupLayer (CCLayerColor):
//     единая модальность, единые приоритеты тачей, единое безопасное
//     закрытие closeSelf() с защитой от двойного закрытия и с отключением
//     touch/keypad до удаления. Раньше каждый попап дублировал это вручную.
//  5) rebuildList() удалял кнопки списка прямо внутри activate() той же
//     кнопки (Del). Теперь пересборка списка отложена на 0 мс.
//  6) «Save as» обновлял список через CCDelayTime(0.3) — гонка с фоновой
//     записью файла. Теперь статус сохранения атомарный, список обновляется
//     по факту завершения записи (опрос каждые 0.1 с).
//  7) Отсутствовала проверка имени слота: "a/b", "..", "" давали выход за
//     пределы каталога и падение на create_directories/rename. Добавлена
//     санитизация.
//  8) Все колбэки попапов держат Ref<> на владельца вместо сырого this,
//     а отложенные действия планируются на узле, который их переживёт.
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
        uint8_t held;   // накопленная маска после этого события
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
                if (kTpsPresets[i] > cur) { setTps(kTpsPresets[i]); return;
