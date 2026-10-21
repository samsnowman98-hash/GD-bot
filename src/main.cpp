// src/main.cpp
// Geometry Dash 2.2081 / Geode 5.11.2 / C++23
//
// ВАЖНО:
// VERIFY-N — обязательная проверка перед использованием на реальных уровнях.
// Одних сигнатур bindings недостаточно, чтобы доказать, что processCommands
// охватывает ВСЮ интеграцию физического шага. Ниже этот контракт проверяется
// частично: количеством вызовов, расположением checkCollisions и состоянием
// игроков до/после processCommands. При обнаружении несовместимости бот
// останавливается, а не продолжает создавать заведомо неправильный макрос.
//
// Эта проверка — детектор, а не барьер: несовместимость обнаруживается
// ПОСЛЕ того, как нативный код уже выполнил шаг физики с неучтённым нами
// делением на подшаги. Остановка происходит максимально быстро после этого,
// но сам уже выполненный шаг отменить нельзя — поэтому запись за этот шаг
// обрезается (trimRecording), а не просто помечается как «подозрительная».
//
// Никаких изменений отправки результатов, прогресса, античита или рекордов.
//
// Макрос:
//   заголовок 48 байт, записи по 16 байт, CRC32 в конце;
//   little-endian; double хранится как IEEE-754 binary64;
//   фаза [0, 1), фаза 1 канонически представляется следующим шагом, фазой 0.
//
// Сохранение файла атомарно в пределах одной файловой системы: сначала
// пишется во временный файл в той же папке, затем временный файл
// ПЕРЕИМЕНОВЫВАЕТСЯ в целевое имя (fs::rename). Если процесс будет прерван
// на Android в середине записи, под финальным именем никогда не окажется
// частично записанный файл — он либо не появится вовсе (и останется только
// .tmp-мусор, который безопасно удалить), либо появится полностью целым.
//
// Кириллица:
//   интерфейс рисуется CCLabelTTF, а НЕ стандартными bigFont/chatFont;
//   TextInput используется только для чисел и ASCII-имени файла.
//
// Шаг назад намеренно недоступен. Непроверенные снимки состояния не выдаются
// за полноценное восстановление RNG, триггеров, действий и состояния модов.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayerObject.hpp>

#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>

#include <Geode/utils/async.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/file.hpp>

// VERIFY-6: путь FMOD-заголовка в установленном SDK.
#if __has_include(<Geode/fmod/fmod.hpp>)
# include <Geode/fmod/fmod.hpp>
#elif __has_include(<fmod.hpp>)
# include <fmod.hpp>
#else
# error "Проверьте путь к FMOD C++ headers в Geode SDK"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace gdbot {
    namespace fs = std::filesystem;

    constexpr std::uint32_t kDefaultTPS = 240;
    constexpr std::uint32_t kMinTPS = 30;
    constexpr std::uint32_t kMaxTPS = 2000;
    constexpr std::uint32_t kMaxBurst = 512;
    constexpr std::uint64_t kMaxEvents = 2'000'000;
    constexpr std::uint32_t kHeaderSize = 48;
    constexpr std::uint32_t kRecordSize = 16;
    constexpr std::uint32_t kFormatVersion = 1;
    constexpr std::uint32_t kFooterSize = 4;
    constexpr std::size_t kListLimit = 128;

    constexpr std::array<std::uint8_t, 8> kMagic{
        'G', 'D', 'B', 'O', 'T', '\r', '\n', 0x1a
    };

    static_assert(sizeof(double) == 8);
    static_assert(std::numeric_limits<double>::is_iec559);

    enum class Mode {
        Off,
        Record,
        Playback
    };

    enum class Source {
        Macro,
        Manual
    };

    struct Action {
        std::uint32_t step = 0;
        double phase = 0.0;
        std::uint8_t button = 1;
        bool down = false;
        bool player2 = false;
    };

    struct Macro {
        std::uint32_t tps = kDefaultTPS;
        std::uint64_t level = 0;
        std::uint32_t length = 0;
        std::uint8_t initialHeld = 0;
        std::vector<Action> actions;
    };

    struct RawInput {
        double timestamp = 0.0;
        std::uint8_t button = 1;
        bool down = false;
        bool player2 = false;
        std::uint64_t order = 0;
    };

    struct ScheduledAction {
        Action action;
        Source source = Source::Manual;
        std::uint64_t order = 0;
    };

    struct CheckpointData {
        std::uint32_t nextStep = 0;
        std::uint8_t manualHeld = 0;
        std::uint8_t macroHeld = 0;
        std::uint8_t effectiveHeld = 0;
        bool safeBoundary = false;
    };

    struct PlayerStamp {
        bool exists = false;
        float x = 0.0f;
        float y = 0.0f;
        double velocity = 0.0;
        bool grounded = false;
        bool upsideDown = false;
        bool dart = false;

        bool operator==(PlayerStamp const&) const = default;
    };

    struct PhysicsStamp {
        PlayerStamp p1;
        PlayerStamp p2;
        bool dual = false;

        bool operator==(PhysicsStamp const&) const = default;
    };

    struct Runtime {
        PlayLayer* play = nullptr;
        std::uint64_t session = 0;
        std::uint64_t epoch = 0;
        std::uint64_t level = 0;

        Mode mode = Mode::Off;
        std::optional<Mode> pendingStart;

        std::uint32_t step = 0;
        std::size_t playbackCursor = 0;
        long double debt = 0.0L;

        bool paused = false;
        bool dead = false;
        bool frameMode = false;
        std::uint64_t frameBudget = 0;

        double inputAnchor = 0.0;
        std::uint64_t inputOrder = 0;
        std::vector<RawInput> rawInputs;
        std::map<std::uint32_t, std::vector<ScheduledAction>> manualActions;

        std::uint8_t manualHeld = 0;
        std::uint8_t macroHeld = 0;
        std::uint8_t sentHeld = 0;

        bool flagsSaved = false;
        bool oldBetweenSteps = false;
        bool oldOnSteps = false;

        bool injecting = false;
        bool pauseGuard = false;
        bool inTick = false;
        bool inSlice = false;
        bool commandsDone = false;
        bool badKernel = false;
        unsigned commandCalls = 0;

        PhysicsStamp beforeTick;
        PhysicsStamp afterCommands;

        int resetDepth = 0;
        int loadDepth = 0;
        bool internalRestart = false;
        bool loadedDuringReset = false;
        std::optional<CheckpointData> restoreAfterReset;

        bool markPending = false;
        bool drainingMark = false;

        // Невладеющие ключи. Объекты принадлежат игре.
        // Карта очищается/прореживается при сбросе, store/remove и onQuit.
        std::map<CheckpointObject*, CheckpointData> checkpoints;

        bool stopRequested = false;
        bool disableFramesOnStop = false;
        bool discardCurrentStep = false;
        bool questionAfterStop = false;
        std::string stopMessage;

        bool badTimestampWarned = false;
    };

    struct AudioState {
        bool pitchOwned = false;
        bool pauseOwned = false;
        bool previousMasterPause = false;
        bool failureWarned = false;
    };

    struct Notice {
        std::string text;
        bool warning = false;
    };

    using ChoiceAction = std::pair<std::string, std::function<void()>>;

    struct State {
        bool configured = false;
        std::uint32_t tps = kDefaultTPS;
        double recordingSpeed = 1.0;
        bool blockInput = false;
        std::uint32_t frameSize = 1;

        fs::path directory;
        std::string fileStem = "macro";

        Macro buffer;
        bool hasBuffer = false;

        Runtime run;
        AudioState audio;

        std::uint64_t nextSession = 0;
        std::uint64_t temporarySequence = 0;
        std::uint64_t uiRevision = 0;

        int modalCount = 0;
        bool deathQuestion = false;
        std::uint64_t deathQuestionSession = 0;

        std::vector<Notice> notices;
    };

    State& state() {
        // Только обычная C++-инициализация. Mod::get() здесь не вызывается.
        static State value;
        return value;
    }

    void showNotice(std::string const& text, bool warning = false);
    void ask(std::string text, std::vector<ChoiceAction> actions);
    void openPanel();
    void chooseFolder();
    void useModFolder();
    void pumpUI();
    void syncAudio();
    void stopNow(bool disableFrames);
    void flushStop();
    void beginPendingStart();

    void changed() {
        ++state().uiRevision;
    }

    void postNotice(std::string text, bool warning = false) {
        auto& messages = state().notices;
        if (messages.size() < 16)
            messages.push_back({std::move(text), warning});
    }

    std::string pathText(fs::path const& path) {
        auto value = path.u8string();
        return std::string(
            reinterpret_cast<char const*>(value.data()), value.size()
        );
    }

    fs::path utf8Path(std::string const& text) {
        return fs::u8path(text);
    }

    std::string shorten(std::string const& text, std::size_t maxBytes) {
        if (text.size() <= maxBytes)
            return text;

        auto count = maxBytes;
        while (count > 0 &&
               (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80) {
            --count;
        }
        return text.substr(0, count) + "…";
    }

    bool parseUnsigned(
        std::string const& text,
        std::uint32_t minimum,
        std::uint32_t maximum,
        std::uint32_t& result
    ) {
        if (text.empty())
            return false;

        std::uint32_t value = 0;
        auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), value
        );
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() ||
            value < minimum || value > maximum) {
            return false;
        }
        result = value;
        return true;
    }

    bool parseSpeed(std::string text, double& result) {
        std::replace(text.begin(), text.end(), ',', '.');

        // Не зависит от системного десятичного разделителя.
        // Не требует floating-point from_chars в Android libc++.
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());

        double value = 0.0;
        stream >> value;
        if (!stream || !stream.eof() ||
            !std::isfinite(value) || value < 0.01 || value > 2.0) {
            return false;
        }
        result = value;
        return true;
    }

    bool active() {
        auto const& r = state().run;
        return r.play && (r.mode != Mode::Off || r.frameMode);
    }

    bool controls(GJBaseGameLayer const* layer) {
        auto const& r = state().run;
        return active() &&
               static_cast<GJBaseGameLayer const*>(r.play) == layer;
    }

    bool owns(PlayLayer const* layer) {
        return state().run.play == layer;
    }

    double inputTimestamp() {
        // VERIFY-2:
        // - возвращаемый тип getInputTimestamp;
        // - одна ли шкала/эпоха с timestamp в queueButton;
        // - соответствует ли интервал этой шкалы переданному update(dt).
        //
        // Абсолютное значение не используется для физики/воспроизведения.
        return static_cast<double>(geode::utils::getInputTimestamp());
    }

    std::uint64_t levelID(GJGameLevel* level) {
        if (!level)
            return 0;

        // VERIFY-3: m_levelID и .value() в SDK/bindings 2.2081.
        auto value = level->m_levelID.value();
        return value > 0 ? static_cast<std::uint64_t>(value) : 0;
    }

    std::uint8_t buttonBit(int button, bool player2) {
        // VERIFY-3: численные значения кнопок:
        // 1 — прыжок, 2 — влево, 3 — вправо.
        if (button < 1 || button > 3)
            return 0;

        auto index = static_cast<unsigned>(button - 1) + (player2 ? 3u : 0u);
        return static_cast<std::uint8_t>(1u << index);
    }

    // ---------------------------------------------------------------------
    // Бинарный формат и проверка файлов
    // ---------------------------------------------------------------------

    constexpr auto makeCRCTable() {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^
                        ((value & 1) ? 0xedb88320u : 0u);
            result[i] = value;
        }
        return result;
    }

    constexpr auto kCRCTable = makeCRCTable();

    std::uint32_t crcByte(std::uint32_t crc, std::uint8_t value) {
        return (crc >> 8) ^ kCRCTable[(crc ^ value) & 0xffu];
    }

    class BinaryWriter {
    public:
        std::ofstream stream;
        std::uint32_t crc = 0xffffffffu;

        explicit BinaryWriter(fs::path const& path)
          : stream(path, std::ios::binary | std::ios::trunc) {}

        void byte(std::uint8_t value) {
            stream.put(static_cast<char>(value));
            crc = crcByte(crc, value);
        }

        template <class T>
        void integer(T value) {
            static_assert(std::is_unsigned_v<T>);
            for (unsigned i = 0; i < sizeof(T); ++i)
                byte(static_cast<std::uint8_t>(value >> (8 * i)));
        }

        bool finish() {
            auto value = ~crc;
            for (unsigned i = 0; i < 4; ++i)
                stream.put(static_cast<char>(value >> (8 * i)));

            stream.flush();
            bool ok = static_cast<bool>(stream);
            stream.close();
            return ok && !stream.fail();
        }
    };

    class BinaryReader {
    public:
        std::ifstream stream;
        std::uint32_t crc = 0xffffffffu;

        explicit BinaryReader(fs::path const& path)
          : stream(path, std::ios::binary) {}

        bool byte(std::uint8_t& value) {
            int read = stream.get();
            if (read == std::char_traits<char>::eof())
                return false;
            value = static_cast<std::uint8_t>(read);
            crc = crcByte(crc, value);
            return true;
        }

        template <class T>
        bool integer(T& value) {
            static_assert(std::is_unsigned_v<T>);
            value = 0;
            for (unsigned i = 0; i < sizeof(T); ++i) {
                std::uint8_t b = 0;
                if (!byte(b))
                    return false;
                value |= static_cast<T>(b) << (8 * i);
            }
            return true;
        }

        bool finish() {
            std::uint32_t stored = 0;
            for (unsigned i = 0; i < 4; ++i) {
                int b = stream.get();
                if (b == std::char_traits<char>::eof())
                    return false;
                stored |= static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(b)
                ) << (8 * i);
            }
            return stored == ~crc &&
                   stream.peek() == std::char_traits<char>::eof();
        }
    };

    bool actionLess(Action const& a, Action const& b) {
        if (a.step != b.step)
            return a.step < b.step;
        return a.phase < b.phase;
    }

    bool validateMacro(Macro const& macro, std::string& error) {
        if (macro.tps < kMinTPS || macro.tps > kMaxTPS ||
            (macro.initialHeld & ~0x3fu) != 0 ||
            macro.actions.size() > kMaxEvents) {
            error = "Недопустимые параметры макроса.";
            return false;
        }

        std::optional<Action> previous;
        for (auto const& action : macro.actions) {
            if (action.button < 1 || action.button > 3 ||
                !std::isfinite(action.phase) ||
                action.phase < 0.0 || action.phase >= 1.0 ||
                action.step >= macro.length) {
                error = "Повреждена запись действия.";
                return false;
            }
            if (previous && actionLess(action, *previous)) {
                error = "Действия расположены не по порядку.";
                return false;
            }
            previous = action;
        }
        return true;
    }

    bool writeMacro(
        fs::path const& path, Macro const& macro, std::string& error
    ) {
        if (!validateMacro(macro, error))
            return false;

        BinaryWriter writer(path);
        if (!writer.stream) {
            error = "Не удалось создать файл.";
            return false;
        }

        for (auto b : kMagic)
            writer.byte(b);

        writer.integer<std::uint32_t>(kFormatVersion);
        writer.integer<std::uint32_t>(kHeaderSize);
        writer.integer<std::uint32_t>(macro.tps);
        writer.integer<std::uint32_t>(kRecordSize);
        writer.integer<std::uint64_t>(macro.level);
        writer.integer<std::uint32_t>(macro.length);
        writer.integer<std::uint32_t>(macro.initialHeld);
        writer.integer<std::uint64_t>(macro.actions.size());

        for (auto const& action : macro.actions) {
            writer.integer<std::uint32_t>(action.step);
            writer.integer<std::uint64_t>(
                std::bit_cast<std::uint64_t>(action.phase)
            );
            writer.byte(action.button);
            writer.byte(action.down ? 1 : 0);
            writer.byte(action.player2 ? 1 : 0);
            writer.byte(0);
        }

        if (!writer.finish()) {
            error = "Ошибка записи. Проверьте свободное место и доступ.";
            return false;
        }
        return true;
    }

    bool readMacro(
        fs::path const& path, Macro& result, std::string& error
    ) {
        std::error_code ec;
        auto size = fs::file_size(path, ec);
        constexpr auto maxSize =
            std::uint64_t{kHeaderSize} +
            kMaxEvents * kRecordSize + kFooterSize;

        if (ec || size < kHeaderSize + kFooterSize || size > maxSize) {
            error = "Недопустимый размер файла.";
            return false;
        }

        BinaryReader reader(path);
        if (!reader.stream) {
            error = "Не удалось открыть файл.";
            return false;
        }

        for (auto expected : kMagic) {
            std::uint8_t actual = 0;
            if (!reader.byte(actual) || actual != expected) {
                error = "Это не файл макроса этого бота.";
                return false;
            }
        }

        std::uint32_t version = 0;
        std::uint32_t headerSize = 0;
        std::uint32_t recordSize = 0;
        std::uint32_t initial = 0;
        std::uint64_t count = 0;
        Macro candidate;

        if (!reader.integer(version) ||
            !reader.integer(headerSize) ||
            !reader.integer(candidate.tps) ||
            !reader.integer(recordSize) ||
            !reader.integer(candidate.level) ||
            !reader.integer(candidate.length) ||
            !reader.integer(initial) ||
            !reader.integer(count)) {
            error = "Заголовок обрезан.";
            return false;
        }

        if (version != kFormatVersion ||
            headerSize != kHeaderSize ||
            recordSize != kRecordSize) {
            error = "Версия формата не поддерживается.";
            return false;
        }
        if (initial > 0x3f || count > kMaxEvents) {
            error = "Повреждены поля заголовка.";
            return false;
        }

        auto expectedSize =
            std::uint64_t{kHeaderSize} + count * kRecordSize + kFooterSize;
        if (size != expectedSize) {
            error = "Размер файла не соответствует заголовку.";
            return false;
        }

        candidate.initialHeld = static_cast<std::uint8_t>(initial);

        try {
            candidate.actions.reserve(static_cast<std::size_t>(count));
        }
        catch (std::exception const&) {
            error = "Недостаточно памяти для этого макроса.";
            return false;
        }

        for (std::uint64_t i = 0; i < count; ++i) {
            Action action;
            std::uint64_t phaseBits = 0;
            std::uint8_t down = 0;
            std::uint8_t player = 0;
            std::uint8_t reserved = 0;

            if (!reader.integer(action.step) ||
                !reader.integer(phaseBits) ||
                !reader.byte(action.button) ||
                !reader.byte(down) ||
                !reader.byte(player) ||
                !reader.byte(reserved)) {
                error = "Файл обрезан внутри списка действий.";
                return false;
            }

            if (down > 1 || player > 1 || reserved != 0) {
                error = "Повреждены флаги действия.";
                return false;
            }

            action.phase = std::bit_cast<double>(phaseBits);
            action.down = down != 0;
            action.player2 = player != 0;

            // Принимаем точную фазу 1 и переводим в канонический вид.
            if (action.phase == 1.0) {
                if (action.step == std::numeric_limits<std::uint32_t>::max()) {
                    error = "Переполнение номера шага.";
                    return false;
                }
                ++action.step;
                action.phase = 0.0;
            }

            candidate.actions.push_back(action);
        }

        if (!reader.finish()) {
            error = "Не совпала контрольная сумма или файл изменился.";
            return false;
        }
        if (!validateMacro(candidate, error))
            return false;

        result = std::move(candidate);
        return true;
    }

    std::string cleanStem(std::string const& input) {
        std::string result;
        result.reserve(64);

        for (unsigned char c : input) {
            bool allowed =
                (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == ' ';

            if (allowed && result.size() < 64)
                result.push_back(static_cast<char>(c));
        }

        while (!result.empty() && result.back() == ' ')
            result.pop_back();
        while (!result.empty() && result.front() == ' ')
            result.erase(result.begin());

        if (result.empty())
            result = "macro";

        std::string upper = result;
        for (char& c : upper) {
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        }

        bool reserved =
            upper == "CON" || upper == "PRN" ||
            upper == "AUX" || upper == "NUL";

        if (upper.size() == 4 &&
            (upper.starts_with("COM") || upper.starts_with("LPT")) &&
            upper[3] >= '1' && upper[3] <= '9') {
            reserved = true;
        }

        if (reserved)
            result.insert(result.begin(), '_');

        return result;
    }

    bool probeFolder(
        fs::path const& parent, fs::path& result, std::string& error
    ) {
        std::error_code ec;
        auto directory = parent / "GDBotMacros";

        fs::create_directories(directory, ec);
        if (ec || !fs::is_directory(directory, ec) || ec) {
            error = "Не удалось создать подпапку макросов.";
            return false;
        }

        auto test = directory / (
            ".gdbot-write-test-" +
            std::to_string(++state().temporarySequence)
        );

        std::ofstream stream(test, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Система не разрешила запись в выбранную папку.";
            return false;
        }

        stream.write("GDBOT", 5);
        stream.flush();
        bool ok = static_cast<bool>(stream);
        stream.close();
        ok = ok && !stream.fail();

        std::error_code removeError;
        fs::remove(test, removeError);

        if (!ok || removeError) {
            error = "Проверка записи и удаления тестового файла не пройдена.";
            return false;
        }

        result = std::move(directory);
        return true;
    }

    void ensureConfiguration() {
        auto& s = state();
        if (s.configured)
            return;

        s.configured = true;

        auto* mod = Mod::get();
        auto tps = mod->getSavedValue<int>("physics-tps", kDefaultTPS);
        s.tps = static_cast<std::uint32_t>(
            std::clamp(tps, static_cast<int>(kMinTPS), static_cast<int>(kMaxTPS))
        );

        s.recordingSpeed =
            mod->getSavedValue<double>("recording-speed", 1.0);
        if (!std::isfinite(s.recordingSpeed))
            s.recordingSpeed = 1.0;
        s.recordingSpeed = std::clamp(s.recordingSpeed, 0.01, 2.0);

        s.blockInput = mod->getSavedValue<bool>("block-input", false);

        auto frameSize = mod->getSavedValue<int>("frame-size", 1);
        s.frameSize = static_cast<std::uint32_t>(
            std::clamp(frameSize, 1, 10000)
        );

        s.fileStem = cleanStem(
            mod->getSavedValue<std::string>("macro-name", "macro")
        );

        auto storedParent =
            mod->getSavedValue<std::string>("macro-parent", "");

        fs::path parent = storedParent.empty()
            ? mod->getSaveDir()
            : utf8Path(storedParent);

        std::string error;
        if (!probeFolder(parent, s.directory, error)) {
            s.directory.clear();
            postNotice(
                "Папка недоступна. Выберите другую папку в панели бота.",
                true
            );
        }
    }

    // ---------------------------------------------------------------------
    // ПАТЧ 1 / сохранение макроса.
    //
    // Было: writeMacro() во временный файл, затем fs::copy_file() поверх
    // финального имени. copy_file() НЕ атомарна: если процесс убьют (на
    // Android это штатная ситуация — OOM-killer, выгрузка активности)
    // в середине копирования, под финальным именем .gdbot окажется
    // наполовину скопированный, то есть БИТЫЙ, файл — ровно то, от чего
    // этот бинарный формат с CRC32 пытался защититься.
    //
    // Стало: writeMacro() во временный файл в ТОЙ ЖЕ папке, затем
    // fs::rename() временного файла в финальное имя. В пределах одной
    // файловой системы rename — атомарная операция на всех трёх целевых
    // платформах (Windows/Android/десктопный Linux под WSL и т.п.):
    // наблюдатель либо видит старое состояние (файла с таким именем нет),
    // либо новое (файл полностью записан), но никогда — частично записанный
    // файл под финальным именем.
    // ---------------------------------------------------------------------

    void saveBuffer() {
        auto& s = state();
        ensureConfiguration();

        if (!s.hasBuffer) {
            showNotice("В буфере нет макроса.", true);
            return;
        }
        if (s.run.inTick) {
            showNotice("Сохранение доступно между физическими шагами.", true);
            return;
        }
        if (s.directory.empty()) {
            ask(
                "Папка макросов недоступна. Выберите другую папку "
                "или используйте папку мода.",
                {
                    {"Выбрать папку", [] { chooseFolder(); }},
                    {"Папка мода", [] { useModFolder(); }}
                }
            );
            return;
        }

        std::string error;
        fs::path verified;
        if (!probeFolder(s.directory.parent_path(), verified, error)) {
            ask(
                "Запись в папку больше недоступна. На телефоне доступ "
                "может быть отозван системой.",
                {
                    {"Выбрать другую", [] { chooseFolder(); }},
                    {"Папка мода", [] { useModFolder(); }}
                }
            );
            return;
        }

        auto stem = cleanStem(s.fileStem);
        auto temporary = s.directory / (
            ".gdbot-save-" +
            std::to_string(++s.temporarySequence) + ".tmp"
        );

        if (!writeMacro(temporary, s.buffer, error)) {
            std::error_code ec;
            fs::remove(temporary, ec);
            showNotice(error, true);
            return;
        }

        bool saved = false;
        fs::path target;
        std::error_code lastError;

        for (unsigned suffix = 0; suffix < 10000; ++suffix) {
            auto name = stem;
            if (suffix)
                name += "_" + std::to_string(suffix);
            target = s.directory / (name + ".gdbot");

            std::error_code existsError;
            if (fs::exists(target, existsError)) {
                if (existsError) {
                    lastError = existsError;
                    break;
                }
                continue;
            }

            // Атомарное переименование внутри одной папки/ФС.
            // Если имя тем временем заняли (гонка с другим процессом/вкладкой),
            // rename завершится ошибкой file_exists/already_exists, и цикл
            // перейдёт к следующему суффиксу, не трогая временный файл.
            std::error_code renameError;
            fs::rename(temporary, target, renameError);

            if (!renameError) {
                saved = true;
                break;
            }

            lastError = renameError;
            if (renameError != std::errc::file_exists)
                break;
        }

        if (!saved) {
            std::error_code cleanup;
            fs::remove(temporary, cleanup);
            log::warn("Не удалось сохранить макрос: {}", lastError.message());
            showNotice("Не удалось сохранить файл. Проверьте доступ и место.", true);
            return;
        }

        Mod::get()->setSavedValue("macro-name", stem);
        changed();
        showNotice("Макрос сохранён: " + pathText(target.filename()));
    }

    std::vector<fs::path> listMacros() {
        std::vector<fs::path> result;
        auto const& directory = state().directory;
        if (directory.empty())
            return result;

        std::error_code ec;
        fs::directory_iterator iterator(directory, ec);
        fs::directory_iterator end;

        while (!ec && iterator != end && result.size() < kListLimit) {
            auto const& entry = *iterator;
            std::error_code entryError;
            if (entry.is_regular_file(entryError) && !entryError &&
                entry.path().extension() == ".gdbot") {
                result.push_back(entry.path());
            }
            iterator.increment(ec);
        }

        std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
            return pathText(a.filename()) < pathText(b.filename());
        });
        return result;
    }

    bool compatible(Macro const& macro, std::string& error) {
        auto const& s = state();
        if (!s.run.play) {
            error = "Сначала откройте уровень.";
            return false;
        }
        if (macro.tps != s.tps) {
            error = "Частота физики макроса не совпадает с настройкой бота.";
            return false;
        }
        if (macro.level != s.run.level) {
            error = "Макрос относится к другому уровню.";
            return false;
        }
        return true;
    }

    bool loadFile(fs::path const& path) {
        auto& s = state();

        if (s.run.mode != Mode::Off || s.run.pendingStart) {
            showNotice("Перед загрузкой выключите запись и воспроизведение.", true);
            return false;
        }

        Macro candidate;
        std::string error;
        if (!readMacro(path, candidate, error) ||
            !compatible(candidate, error)) {
            showNotice(error, true);
            return false;
        }

        s.buffer = std::move(candidate);
        s.hasBuffer = true;
        s.fileStem = cleanStem(pathText(path.stem()));
        s.run.playbackCursor = 0;
        s.run.checkpoints.clear();
        changed();

        if (s.buffer.level == 0) {
            showNotice(
                "Макрос загружен. Локальные уровни без номера не различаются.",
                true
            );
        }
        else {
            showNotice("Макрос загружен.");
        }
        return true;
    }

    // ---------------------------------------------------------------------
    // Звук
    // ---------------------------------------------------------------------

    FMOD::ChannelGroup* masterGroup() {
        // VERIFY-6:
        // sharedEngine()->m_system в bindings 2.2081.
        // Используется мастер-группа FMOD, без выдуманных полей каналов музыки.
        auto* engine = FMODAudioEngine::sharedEngine();
        if (!engine || !engine->m_system)
            return nullptr;

        FMOD::ChannelGroup* group = nullptr;
        if (engine->m_system->getMasterChannelGroup(&group) != FMOD_OK)
            return nullptr;
        return group;
    }

    void audioFailure() {
        auto& audio = state().audio;
        if (!audio.failureWarned) {
            audio.failureWarned = true;
            postNotice("Не удалось изменить скорость звука.", true);
        }
    }

    void restoreAudio() {
        auto& audio = state().audio;
        auto* group = masterGroup();
        if (group) {
            if (audio.pitchOwned)
                group->setPitch(1.0f);
            if (audio.pauseOwned)
                group->setPaused(audio.previousMasterPause);
        }
        audio.pitchOwned = false;
        audio.pauseOwned = false;
    }

    void syncAudio() {
        auto& s = state();
        auto& r = s.run;
        auto& audio = s.audio;

        bool paused = !r.play || r.paused ||
                      (r.play && r.play->m_isPaused) || r.dead;

        bool recording = r.mode == Mode::Record && !paused;
        bool playback = r.mode == Mode::Playback && !paused;

        if (!recording && !playback && !r.frameMode) {
            restoreAudio();
            return;
        }

        auto* group = masterGroup();
        if (!group) {
            audioFailure();
            return;
        }

        float pitch = recording
            ? static_cast<float>(s.recordingSpeed)
            : 1.0f;

        // VERIFY-6: проверить фактическое изменение темпа/тона на всех платформах.
        // Мастер-группа распространяет изменение и на звуковые эффекты.
        if (group->setPitch(pitch) != FMOD_OK)
            audioFailure();
        else
            audio.pitchOwned = true;

        // В экспериментальном покадровом режиме аудио не должно убегать
        // на минуты вперёд, пока физика неподвижна.
        bool freezeAudio = r.frameMode && !paused && active();

        if (freezeAudio && !audio.pauseOwned) {
            bool previous = false;
            if (group->getPaused(&previous) == FMOD_OK &&
                group->setPaused(true) == FMOD_OK) {
                audio.previousMasterPause = previous;
                audio.pauseOwned = true;
            }
            else {
                audioFailure();
            }
        }
        else if (!freezeAudio && audio.pauseOwned) {
            group->setPaused(audio.previousMasterPause);
            audio.pauseOwned = false;
        }
    }

    // ---------------------------------------------------------------------
    // Ввод, удержания и управление сеансом
    // ---------------------------------------------------------------------

    std::uint8_t effectiveHeld() {
        auto const& s = state();
        auto const& r = s.run;

        if (r.mode == Mode::Playback) {
            return static_cast<std::uint8_t>(
                r.macroHeld | (s.blockInput ? 0 : r.manualHeld)
            );
        }
        return r.manualHeld;
    }

    void sendButton(int button, bool down, bool player2) {
        auto& r = state().run;
        if (!r.play)
            return;

        bool previous = r.injecting;
        r.injecting = true;
        r.play->handleButton(down, button, !player2);
        r.injecting = previous;
    }

    void sendMask(std::uint8_t desired, bool force = false) {
        auto& r = state().run;
        if (!r.play)
            return;

        auto difference = force
            ? static_cast<std::uint8_t>(0x3f)
            : static_cast<std::uint8_t>(desired ^ r.sentHeld);

        for (int player = 0; player < 2; ++player) {
            for (int button = 1; button <= 3; ++button) {
                auto bit = buttonBit(button, player != 0);
                if (difference & bit)
                    sendButton(button, (desired & bit) != 0, player != 0);
            }
        }
        r.sentHeld = desired;
    }

    void installNativeFlags() {
        auto& r = state().run;
        if (!r.play)
            return;

        if (!r.flagsSaved) {
            r.oldBetweenSteps = r.play->m_clickBetweenSteps;
            r.oldOnSteps = r.play->m_clickOnSteps;
            r.flagsSaved = true;
        }

        r.play->m_clickBetweenSteps = false;
        r.play->m_clickOnSteps = false;
        r.play->m_queuedButtons.clear();
    }

    void restoreNativeFlags() {
        auto& r = state().run;
        if (r.play && r.flagsSaved) {
            r.play->m_clickBetweenSteps = r.oldBetweenSteps;
            r.play->m_clickOnSteps = r.oldOnSteps;
            r.play->m_queuedButtons.clear();
        }
        r.flagsSaved = false;
    }

    void requestStop(
        std::string message,
        bool question = false,
        bool fatal = false
    ) {
        auto& r = state().run;
        r.stopRequested = true;
        r.questionAfterStop = r.questionAfterStop || question;
        r.disableFramesOnStop = r.disableFramesOnStop || fatal;

        if (r.stopMessage.empty() || fatal)
            r.stopMessage = std::move(message);
    }

    void stopNow(bool disableFrames) {
        auto& s = state();
        auto& r = s.run;

        r.pendingStart.reset();
        r.mode = Mode::Off;
        r.macroHeld = 0;

        if (!r.dead && r.play)
            sendMask(r.manualHeld);

        if (disableFrames) {
            r.frameMode = false;
            r.frameBudget = 0;
        }

        r.rawInputs.clear();
        r.manualActions.clear();
        r.debt = 0.0L;
        r.markPending = false;

        if (!active())
            restoreNativeFlags();

        syncAudio();
        changed();
    }

    void flushStop() {
        auto& s = state();
        auto& r = s.run;
        if (!r.stopRequested)
            return;

        auto message = std::move(r.stopMessage);
        bool question = r.questionAfterStop;
        bool disableFrames = r.disableFramesOnStop;

        r.stopRequested = false;
        r.questionAfterStop = false;
        r.disableFramesOnStop = false;
        r.stopMessage.clear();

        stopNow(disableFrames);

        if (!message.empty())
            postNotice(std::move(message), question || disableFrames);

        if (question) {
            s.deathQuestion = true;
            s.deathQuestionSession = r.session;
        }
    }

    void trimRecording(std::uint32_t nextStep) {
        auto& s = state();
        if (!s.hasBuffer)
            return;

        auto& actions = s.buffer.actions;
        auto first = std::lower_bound(
            actions.begin(), actions.end(), nextStep,
            [](Action const& action, std::uint32_t step) {
                return action.step < step;
            }
        );
        actions.erase(first, actions.end());
        s.buffer.length = nextStep;
    }

    void clearTimeline(std::uint32_t step = 0) {
        auto& r = state().run;
        ++r.epoch;
        r.step = step;
        r.playbackCursor = 0;
        r.debt = 0.0L;
        r.frameBudget = 0;
        r.rawInputs.clear();
        r.manualActions.clear();
        r.manualHeld = 0;
        r.macroHeld = 0;
        r.sentHeld = 0;
        r.dead = false;
        r.markPending = false;
        r.inputAnchor = inputTimestamp();
    }

    void requestStart(Mode mode) {
        auto& s = state();
        auto& r = s.run;

        if (!r.play || mode == Mode::Off)
            return;

        if (mode == Mode::Playback) {
            std::string error;
            if (!s.hasBuffer || s.buffer.length == 0) {
                showNotice("В буфере нет непустого макроса.", true);
                return;
            }
            if (!compatible(s.buffer, error)) {
                showNotice(error, true);
                return;
            }
        }

        stopNow(false);
        r.pendingStart = mode;
        s.deathQuestion = false;
        changed();
        showNotice("Запуск с начала уровня после выхода из паузы.");
    }

    // ---------------------------------------------------------------------
    // ПАТЧ 2 / двойная инициализация таймлайна.
    //
    // Было: play->resetLevelFromStart() синхронно проходит через хук
    // GDBotPlayLayer::resetLevelFromStart(), который вызывает resetBegin()
    // ДО и resetEnd() ПОСЛЕ нативного вызова. resetEnd() видит
    // r.internalRestart == true и САМ делает clearTimeline() +
    // r.checkpoints.clear() (см. ниже по файлу). После возврата из
    // resetLevelFromStart() та же пара вызовов повторялась здесь ещё раз —
    // безвредно по результату, но clearTimeline() попутно переустанавливает
    // r.inputAnchor = inputTimestamp(), поэтому первое, уже выполненное
    // resetEnd()'ом выставление анкера полностью пропадало впустую, и был
    // неочевидный вопрос "кто на самом деле отвечает за инициализацию".
    //
    // Стало: инициализация таймлайна и чекпоинтов после
    // resetLevelFromStart() выполняется ровно один раз — внутри resetEnd()
    // по флагу internalRestart. Здесь остаётся только то, что resetEnd()
    // сознательно не делает (режим бота, флаг паузы, содержимое буфера).
    // ---------------------------------------------------------------------

    void beginPendingStart() {
        auto& s = state();
        auto& r = s.run;
        if (!r.pendingStart || !r.play || r.play->m_isPaused)
            return;

        auto mode = *r.pendingStart;
        auto* play = r.play;
        auto session = r.session;
        r.pendingStart.reset();

        if (mode == Mode::Playback) {
            std::string error;
            if (!s.hasBuffer || !compatible(s.buffer, error)) {
                postNotice(error.empty() ? "Макрос недоступен." : error, true);
                return;
            }
        }

        r.internalRestart = true;
        play->resetLevelFromStart();
        if (r.play != play || r.session != session)
            return;
        r.internalRestart = false;

        // clearTimeline() и checkpoints.clear() уже выполнены внутри
        // resetEnd() (см. ветку internalRestart) синхронно во время
        // вызова play->resetLevelFromStart() выше — повторять их здесь
        // не нужно и означало бы бесполезно перевыставлять inputAnchor.

        r.paused = play->m_isPaused;
        r.mode = mode;

        if (mode == Mode::Record) {
            s.buffer = Macro{};
            s.buffer.tps = s.tps;
            s.buffer.level = r.level;
            s.hasBuffer = true;
        }
        else {
            r.macroHeld = s.buffer.initialHeld;
        }

        installNativeFlags();
        sendMask(effectiveHeld(), true);
        syncAudio();
        changed();
    }

    void enqueueRaw(
        bool down, int button, bool player2, double timestamp
    ) {
        auto& r = state().run;
        if (!buttonBit(button, player2))
            return;
        if (!r.play || r.pauseGuard || r.paused ||
            r.play->m_isPaused || r.dead || r.pendingStart) {
            return;
        }

        if (r.rawInputs.size() >= 65536) {
            requestStop("Переполнена очередь ввода. Бот остановлен.", false, true);
            return;
        }

        r.rawInputs.push_back({
            timestamp,
            static_cast<std::uint8_t>(button),
            down,
            player2,
            ++r.inputOrder
        });
    }

    void mapRawInputs(double frameDelta, double inputEnd) {
        auto& s = state();
        auto& r = s.run;

        double inputSpan = inputEnd - r.inputAnchor;
        long double origin =
            static_cast<long double>(r.step) + r.debt * s.tps;

        double speed = r.mode == Mode::Record ? s.recordingSpeed : 1.0;
        long double span =
            static_cast<long double>(frameDelta) * speed * s.tps;

        for (auto const& input : r.rawInputs) {
            long double tick = r.step;

            if (!r.frameMode) {
                double ratio = 1.0;
                if (std::isfinite(input.timestamp) &&
                    std::isfinite(inputSpan) && inputSpan > 0.0) {
                    ratio = std::clamp(
                        (input.timestamp - r.inputAnchor) / inputSpan,
                        0.0, 1.0
                    );
                }
                else if (!r.badTimestampWarned) {
                    r.badTimestampWarned = true;
                    postNotice(
                        "Шкала времени ввода недоступна: проверьте привязку кликов.",
                        true
                    );
                }

                tick = origin + span * ratio;
            }

            if (!std::isfinite(tick) ||
                tick >= std::numeric_limits<std::uint32_t>::max() - 1.0L) {
                requestStop("Достигнут предел длины макроса.", false, true);
                break;
            }

            tick = std::max(tick, static_cast<long double>(r.step));
            auto integerPart = std::floor(tick);

            Action action;
            action.step = static_cast<std::uint32_t>(integerPart);
            action.phase = static_cast<double>(tick - integerPart);
            action.button = input.button;
            action.down = input.down;
            action.player2 = input.player2;

            if (action.phase >= 1.0) {
                ++action.step;
                action.phase = 0.0;
            }

            r.manualActions[action.step].push_back({
                action, Source::Manual, input.order
            });
        }

        r.rawInputs.clear();
        r.inputAnchor = inputEnd;
    }

    void applyAction(ScheduledAction const& scheduled) {
        auto& s = state();
        auto& r = s.run;
        auto const& action = scheduled.action;
        auto bit = buttonBit(action.button, action.player2);
        if (!bit)
            return;

        auto& sourceMask = scheduled.source == Source::Macro
            ? r.macroHeld
            : r.manualHeld;

        if (action.down)
            sourceMask |= bit;
        else
            sourceMask &= static_cast<std::uint8_t>(~bit);

        auto desired = effectiveHeld();
        bool changesEffectiveState = ((desired ^ r.sentHeld) & bit) != 0;

        if (r.mode == Mode::Record &&
            scheduled.source == Source::Manual &&
            changesEffectiveState) {
            if (s.buffer.actions.size() >= kMaxEvents) {
                r.discardCurrentStep = true;
                requestStop(
                    "Достигнут предел числа действий. Запись остановлена.",
                    false, true
                );
            }
            else {
                Action recorded = action;
                recorded.down = (desired & bit) != 0;
                s.buffer.actions.push_back(recorded);
            }
        }

        sendMask(desired);
    }

    std::vector<ScheduledAction> actionsForStep() {
        auto& s = state();
        auto& r = s.run;
        std::vector<ScheduledAction> result;

        if (r.mode == Mode::Playback) {
            auto const& actions = s.buffer.actions;
            while (r.playbackCursor < actions.size() &&
                   actions[r.playbackCursor].step < r.step) {
                ++r.playbackCursor;
            }

            while (r.playbackCursor < actions.size() &&
                   actions[r.playbackCursor].step == r.step) {
                result.push_back({
                    actions[r.playbackCursor],
                    Source::Macro,
                    static_cast<std::uint64_t>(r.playbackCursor)
                });
                ++r.playbackCursor;
            }
        }

        auto found = r.manualActions.find(r.step);
        if (found != r.manualActions.end()) {
            auto& manual = found->second;
            result.insert(result.end(), manual.begin(), manual.end());
            r.manualActions.erase(found);
        }

        std::stable_sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
            if (a.action.phase != b.action.phase)
                return a.action.phase < b.action.phase;
            if (a.source != b.source)
                return a.source == Source::Macro;
            return a.order < b.order;
        });
        return result;
    }

    // ---------------------------------------------------------------------
    // Практика и восстановление чекпоинтов
    // ---------------------------------------------------------------------

    CheckpointData captureCheckpointData() {
        auto const& r = state().run;
        CheckpointData data;
        data.nextStep = r.step;
        data.safeBoundary = !r.inTick || r.commandsDone;

        if (r.inTick && r.commandsDone)
            ++data.nextStep;

        data.manualHeld = r.manualHeld;
        data.macroHeld = r.macroHeld;
        data.effectiveHeld = r.sentHeld;
        return data;
    }

    void pruneCheckpoints() {
        auto& r = state().run;
        if (!r.play)
            return;

        // VERIFY-4: m_checkpointArray — CCArray*, содержащий действующие CP.
        auto* array = r.play->m_checkpointArray;
        std::erase_if(r.checkpoints, [array](auto const& entry) {
            return !array || !array->containsObject(entry.first);
        });
    }

    bool hasUsableCheckpoint() {
        auto const& r = state().run;
        if (!r.play || !r.play->m_checkpointArray ||
            r.play->m_checkpointArray->count() == 0) {
            return false;
        }

        auto* checkpoint = static_cast<CheckpointObject*>(
            r.play->m_checkpointArray->lastObject()
        );
        auto found = r.checkpoints.find(checkpoint);
        return found != r.checkpoints.end() &&
               found->second.safeBoundary;
    }

    void restoreCheckpointData(std::optional<CheckpointData> const& saved) {
        auto& s = state();
        auto& r = s.run;

        if (!saved || !saved->safeBoundary) {
            if (r.mode != Mode::Off || r.frameMode) {
                requestStop(
                    "Нет безопасного состояния чекпоинта. Бот остановлен.",
                    r.mode == Mode::Record, true
                );
            }
            r.dead = false;
            return;
        }

        auto data = *saved;
        clearTimeline(data.nextStep);

        if (r.mode == Mode::Record)
            trimRecording(data.nextStep);

        r.manualHeld = data.manualHeld;
        r.macroHeld = r.mode == Mode::Playback ? data.macroHeld : 0;
        r.sentHeld = data.effectiveHeld;

        r.playbackCursor = static_cast<std::size_t>(
            std::lower_bound(
                s.buffer.actions.begin(), s.buffer.actions.end(), data.nextStep,
                [](Action const& action, std::uint32_t step) {
                    return action.step < step;
                }
            ) - s.buffer.actions.begin()
        );

        // VERIFY-5: идемпотентность handleButton при повторном выставлении
        // уже восстановленного состояния кнопки. Проверить, что это не создаёт
        // дополнительный фронт прыжка после native loadFromCheckpoint.
        sendMask(effectiveHeld(), true);

        r.dead = false;
        syncAudio();
        changed();
    }

    void resetBegin() {
        auto& r = state().run;
        if (++r.resetDepth == 1) {
            r.loadedDuringReset = false;
            r.restoreAfterReset.reset();
        }
    }

    void resetEnd() {
        auto& s = state();
        auto& r = s.run;
        if (r.resetDepth <= 0 || --r.resetDepth != 0)
            return;

        if (r.internalRestart) {
            clearTimeline();
            r.checkpoints.clear();
            return;
        }

        if (r.loadedDuringReset) {
            auto saved = r.restoreAfterReset;
            r.restoreAfterReset.reset();
            restoreCheckpointData(saved);
        }
        else {
            if (r.mode == Mode::Record) {
                requestStop(
                    "Сброс без чекпоинта: запись оставлена в буфере.",
                    true
                );
            }
            else if (r.mode == Mode::Playback) {
                requestStop("Воспроизведение остановлено сбросом уровня.");
            }

            clearTimeline();
            r.checkpoints.clear();
        }

        if (!r.inTick)
            flushStop();
        syncAudio();
    }

    void onDeath() {
        auto& r = state().run;
        if (r.dead)
            return;

        r.dead = true;
        r.frameBudget = 0;
        r.rawInputs.clear();
        r.manualActions.clear();
        restoreAudio();

        if (r.mode == Mode::Record && !hasUsableCheckpoint()) {
            requestStop(
                "Смерть без подходящего чекпоинта. Запись оставлена в буфере.",
                true
            );
        }
        else if (r.mode == Mode::Playback) {
            requestStop("Воспроизведение остановлено после смерти.");
        }

        if (!r.inTick)
            flushStop();
    }

    // ---------------------------------------------------------------------
    // Фиксированный драйвер и собственные подшаги
    // ---------------------------------------------------------------------

    PlayerStamp stampPlayer(PlayerObject* player) {
        if (!player)
            return {};

        auto position = player->getPosition();
        return {
            true,
            position.x,
            position.y,
            static_cast<double>(player->m_yVelocity),
            player->m_isOnGround,
            player->m_isUpsideDown,
            player->m_isDart
        };
    }

    PhysicsStamp physicsStamp() {
        auto const& r = state().run;
        if (!r.play)
            return {};

        bool dual = r.play->m_gameState.m_isDualMode;
        return {
            stampPlayer(r.play->m_player1),
            dual ? stampPlayer(r.play->m_player2) : PlayerStamp{},
            dual
        };
    }

    template <class Original>
    void splitCommands(
        GJBaseGameLayer* layer,
        float dt,
        bool isHalfTick,
        bool isLastTick,
        Original&& original
    ) {
        auto& r = state().run;

        if (!controls(layer) || !r.inTick ||
            r.resetDepth || r.loadDepth) {
            original(dt, isHalfTick, isLastTick);
            return;
        }

        if (++r.commandCalls != 1) {
            r.badKernel = true;
            original(dt, isHalfTick, isLastTick);
            return;
        }

        // VERIFY-1: processCommands должен быть полным ядром шага,
        // а не только обработчиком триггеров/команд.
        //
        // Единицы dt не угадываются: разделяется именно переданный движком dt,
        // в одинаковых пропорциях при записи и воспроизведении.
        if (!(physicsStamp() == r.beforeTick))
            r.badKernel = true;

        if (r.badKernel) {
            original(dt, isHalfTick, isLastTick);
            r.afterCommands = physicsStamp();
            r.commandsDone = true;
            return;
        }

        auto actions = actionsForStep();
        auto session = r.session;
        auto epoch = r.epoch;
        double position = 0.0;

        auto interrupted = [&] {
            return r.session != session || r.epoch != epoch ||
                   !r.play || r.dead || r.stopRequested;
        };

        auto advance = [&](double end, bool finalSlice) {
            if (end <= position)
                return;

            float slice = static_cast<float>(
                static_cast<double>(dt) * (end - position)
            );
            position = end;

            if (slice <= 0.0f)
                return;

            bool previous = r.inSlice;
            r.inSlice = true;

            // VERIFY-1: семантика isHalfTick/isLastTick при дроблении.
            // Финализация последнего тика разрешена только последнему подшагу.
            original(slice, isHalfTick, isLastTick && finalSlice);

            r.inSlice = previous;
        };

        for (std::size_t i = 0; i < actions.size();) {
            double phase = actions[i].action.phase;
            advance(phase, false);
            if (interrupted())
                break;

            do {
                applyAction(actions[i]);
                ++i;
            } while (i < actions.size() &&
                     actions[i].action.phase == phase);

            if (interrupted())
                break;
        }

        if (!interrupted())
            advance(1.0, true);

        if (r.session == session && r.epoch == epoch && r.play) {
            r.afterCommands = physicsStamp();
            r.commandsDone = !r.dead && !r.stopRequested;
        }
    }

    template <class Original>
    void driveUpdate(
        GJBaseGameLayer* layer, float incomingDelta, Original&& original
    ) {
        auto& s = state();
        auto& r = s.run;

        if (!r.play ||
            static_cast<GJBaseGameLayer*>(r.play) != layer) {
            original(incomingDelta);
            return;
        }

        if (r.pendingStart && !r.play->m_isPaused)
            beginPendingStart();

        if (!controls(layer)) {
            original(incomingDelta);
            return;
        }

        installNativeFlags();

        if (r.play->m_isPaused) {
            r.paused = true;
            restoreAudio();
            return;
        }

        double frameDelta =
            std::isfinite(incomingDelta) && incomingDelta > 0.0f
                ? static_cast<double>(incomingDelta)
                : 0.0;

        if (r.paused) {
            r.paused = false;
            r.inputAnchor = inputTimestamp();
            r.rawInputs.clear();

            // Не включаем длительность паузы в накопитель.
            frameDelta = 0.0;

            // VERIFY-5: native pause может сбрасывать внутренние поля игрока
            // в обход handleButton. Повторное выставление должно быть проверено.
            sendMask(effectiveHeld(), true);
        }

        syncAudio();

        if (r.dead) {
            // Даем нативной логике смерти/возрождения работать.
            // Номера шагов макроса во время ожидания возрождения не идут.
            original(static_cast<float>(frameDelta));
            flushStop();
            return;
        }

        if (r.stopRequested) {
            flushStop();
            if (!controls(layer))
                return;
        }

        auto inputEnd = inputTimestamp();
        mapRawInputs(frameDelta, inputEnd);

        if (r.stopRequested) {
            flushStop();
            return;
        }

        long double quantum = 1.0L / s.tps;
        double speed = r.mode == Mode::Record ? s.recordingSpeed : 1.0;

        if (!r.frameMode)
            r.debt += static_cast<long double>(frameDelta) * speed;

        unsigned executed = 0;
        while (executed < kMaxBurst && controls(layer)) {
            bool ready = r.frameMode
                ? r.frameBudget > 0
                : r.debt + quantum * 1e-10L >= quantum;

            if (!ready || r.dead || r.play->m_isPaused)
                break;

            if (r.step >= std::numeric_limits<std::uint32_t>::max() - 1) {
                requestStop("Достигнут предел длины макроса.", false, true);
                flushStop();
                break;
            }

            if (r.mode == Mode::Playback && r.step >= s.buffer.length) {
                requestStop("Макрос завершён.");
                flushStop();
                break;
            }

            if (r.frameMode)
                --r.frameBudget;
            else
                r.debt = std::max(0.0L, r.debt - quantum);

            auto* play = r.play;
            auto session = r.session;
            auto epoch = r.epoch;
            auto step = r.step;
            bool wasRecording = r.mode == Mode::Record;

            r.inTick = true;
            r.inSlice = false;
            r.commandsDone = false;
            r.commandCalls = 0;
            r.badKernel = false;
            r.discardCurrentStep = false;
            r.beforeTick = physicsStamp();

            // VERIFY-1: GJBaseGameLayer::update(float) как точка внешнего
            // драйвера 2.2081. Вызов оригинала выполняется ОДИН раз на квант.
            original(static_cast<float>(quantum));

            r.inTick = false;
            r.inSlice = false;
            ++executed;

            if (r.play != play || r.session != session)
                return;

            if (r.epoch != epoch) {
                flushStop();
                break;
            }

            if (!r.dead && !r.stopRequested &&
                r.commandCalls == 1 &&
                !(physicsStamp() == r.afterCommands)) {
                r.badKernel = true;
            }

            if ((!r.dead && !r.stopRequested && r.commandCalls != 1) ||
                r.commandCalls > 1) {
                r.badKernel = true;
            }

            if (r.badKernel || r.discardCurrentStep) {
                if (wasRecording)
                    trimRecording(step);

                log::warn(
                    "VERIFY-1: неподтвержденный контракт физического шага; "
                    "processCommands вызван {} раз",
                    r.commandCalls
                );

                requestStop(
                    "Проверка физики не пройдена. Бот безопасно остановлен.",
                    wasRecording, true
                );
                flushStop();
                break;
            }

            if (r.commandCalls == 1) {
                ++r.step;
                if (wasRecording)
                    s.buffer.length = r.step;
            }

            if (r.markPending && !r.dead && !r.stopRequested) {
                r.markPending = false;
                r.drainingMark = true;
                play->markCheckpoint();
                r.drainingMark = false;
            }

            if (r.mode == Mode::Playback && r.step >= s.buffer.length)
                requestStop("Макрос завершён.");

            flushStop();

            if (r.dead)
                break;
        }

        // При достижении kMaxBurst остаток НЕ выбрасывается.
        // Слабое устройство будет отставать, но не пропускать физические шаги.
    }

    void attach(PlayLayer* play, GJGameLevel* level) {
        ensureConfiguration();
        restoreAudio();

        auto& s = state();
        s.run = Runtime{};
        s.run.play = play;
        s.run.session = ++s.nextSession;
        s.run.level = levelID(level);
        s.run.paused = play->m_isPaused;
        s.run.inputAnchor = inputTimestamp();
        s.deathQuestion = false;

        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            postNotice(
                "Загружен внешний мод кликов между кадрами. "
                "Для проверки бота рекомендуется его отключить.",
                true
            );
        }
        changed();
    }

    void detach(PlayLayer* play) {
        auto& s = state();
        if (!owns(play))
            return;

        stopNow(true);
        restoreAudio();
        restoreNativeFlags();
        s.run = Runtime{};
        s.deathQuestion = false;
        changed();
    }

    void setFrameMode(bool enabled) {
        auto& r = state().run;
        if (!r.play)
            return;

        r.frameMode = enabled;
        r.frameBudget = 0;
        r.debt = 0.0L;
        r.inputAnchor = inputTimestamp();

        if (enabled)
            installNativeFlags();
        else if (!active())
            restoreNativeFlags();

        syncAudio();
        changed();
    }

    void advanceFrames() {
        auto& s = state();
        auto& r = s.run;
        if (!r.play || !r.frameMode || r.play->m_isPaused || r.dead)
            return;

        r.frameBudget = std::min<std::uint64_t>(
            r.frameBudget + s.frameSize, 1'000'000
        );
    }

    // ---------------------------------------------------------------------
    // Русский интерфейс
    // ---------------------------------------------------------------------

    CCRect safeRect() {
        auto window = CCDirector::sharedDirector()->getWinSize();

        // VERIFY-7: точное имя/пространство имен safe-area API в SDK 5.11.2.
        // Изолировано в одном адаптере, чтобы исправление не затронуло UI.
        auto area = geode::utils::cocos::getSafeAreaRect();

        if (area.size.width <= 0.0f || area.size.height <= 0.0f)
            area = CCRect{0.0f, 0.0f, window.width, window.height};

        constexpr float padding = 8.0f;
        area.origin.x += padding;
        area.origin.y += padding;
        area.size.width = std::max(1.0f, area.size.width - padding * 2);
        area.size.height = std::max(1.0f, area.size.height - padding * 2);
        return area;
    }

    char const* systemFont() {
        // VERIFY-9: фактическое наличие кириллицы в системном шрифте.
#ifdef GEODE_IS_WINDOWS
        return "Arial";
#else
        return "sans-serif";
#endif
    }

    CCLabelTTF* russianLabel(
        std::string const& text,
        float size = 14.0f,
        float width = 0.0f
    ) {
        if (width > 0.0f) {
            return CCLabelTTF::create(
                text.c_str(), systemFont(), size,
                CCSize{width, 0.0f},
                kCCTextAlignmentLeft,
                kCCVerticalTextAlignmentTop
            );
        }
        return CCLabelTTF::create(text.c_str(), systemFont(), size);
    }

    void fitLabel(CCLabelTTF* label, float width, float height) {
        if (!label)
            return;
        auto size = label->getContentSize();
        if (size.width <= 0 || size.height <= 0)
            return;
        label->setScale(std::min({
            1.0f, width / size.width, height / size.height
        }));
    }

    class CallbackNode final : public CCNode {
        std::function<void()> m_callback;

    public:
        static CallbackNode* create(std::function<void()> callback) {
            auto* node = new CallbackNode;
            if (!node->init()) {
                delete node;
                return nullptr;
            }
            node->m_callback = std::move(callback);
            node->autorelease();
            return node;
        }

        void invoke(CCObject*) {
            // Копия позволяет callback закрыть/удалить владеющую панель.
            auto callback = m_callback;
            if (callback)
                callback();
        }
    };

    CCMenuItemSpriteExtra* textButton(
        CCMenu* menu,
        std::string const& text,
        CCPoint position,
        CCSize size,
        std::function<void()> callback,
        bool enabled = true
    ) {
        auto* normal = CCNode::create();
        normal->setContentSize(size);

        auto* background = CCLayerColor::create(
            enabled ? ccColor4B{43, 75, 108, 235}
                    : ccColor4B{52, 55, 61, 190},
            size.width, size.height
        );
        normal->addChild(background);

        auto* label = russianLabel(text, 14.0f);
        label->setPosition({size.width / 2, size.height / 2});
        fitLabel(label, size.width - 10, size.height - 6);
        if (!enabled)
            label->setColor({140, 145, 150});
        normal->addChild(label);

        auto* handler = CallbackNode::create(std::move(callback));
        auto* item = CCMenuItemSpriteExtra::create(
            normal, handler, menu_selector(CallbackNode::invoke)
        );
        item->addChild(handler);
        item->setPosition(position);
        item->setEnabled(enabled);
        menu->addChild(item);
        return item;
    }

    CCLabelBMFont* findBitmapLabel(CCNode* root) {
        if (auto* label = typeinfo_cast<CCLabelBMFont*>(root))
            return label;

        auto* children = root->getChildren();
        if (!children)
            return nullptr;

        for (unsigned i = 0; i < children->count(); ++i) {
            auto* child = static_cast<CCNode*>(children->objectAtIndex(i));
            if (auto* found = findBitmapLabel(child))
                return found;
        }
        return nullptr;
    }

    void showNotice(std::string const& text, bool warning) {
        auto area = safeRect();

        // Geode Notification сохраняется как контейнер/анимация.
        // Его bitmap-текст заменяется визуально системным TTF-текстом.
        auto placeholderLength = static_cast<std::size_t>(
            std::clamp(area.size.width / 14.0f, 16.0f, 32.0f)
        );
        auto* notification = Notification::create(
            std::string(placeholderLength, 'W'),
            warning ? NotificationIcon::Warning : NotificationIcon::Info,
            3.5f
        );

        // VERIFY-9: Notification создаёт дочерний CCLabelBMFont в init,
        // а его позиция/размер пригодны для замены текста.
        if (auto* bitmap = findBitmapLabel(notification)) {
            auto* parent = bitmap->getParent();
            auto* label = russianLabel(text, 13.0f);
            label->setAnchorPoint(bitmap->getAnchorPoint());
            label->setPosition(bitmap->getPosition());

            float width = std::min(
                area.size.width - 36.0f,
                bitmap->getContentSize().width * bitmap->getScaleX()
            );
            fitLabel(label, std::max(width, 80.0f), 22.0f);
            bitmap->setOpacity(0);
            parent->addChild(label, bitmap->getZOrder() + 1);
            notification->show();
            return;
        }

        // Резервный TTF-тост, если структура Notification изменилась.
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene)
            return;

        float width = std::min(area.size.width, 430.0f);
        auto* toast = CCLayerColor::create(
            {20, 27, 37, 245}, width, 48.0f
        );
        toast->setPosition({
            area.origin.x + (area.size.width - width) / 2,
            area.origin.y + area.size.height - 54.0f
        });

        auto* label = russianLabel(text, 12.0f, width - 18.0f);
        label->setAnchorPoint({0.5f, 0.5f});
        label->setPosition({width / 2, 24.0f});
        fitLabel(label, width - 18.0f, 40.0f);
        toast->addChild(label);
        scene->addChild(toast, 100000);

        toast->runAction(CCSequence::create(
            CCDelayTime::create(3.5f),
            CCRemoveSelf::create(),
            nullptr
        ));
    }

    class ChoicePopup final : public Popup<> {
        std::string m_text;
        std::vector<ChoiceAction> m_actions;

    protected:
        bool setup() override {
            auto size = m_mainLayer->getContentSize();

            auto* title = russianLabel("Бот", 22.0f);
            title->setPosition({size.width / 2, size.height - 24.0f});
            m_mainLayer->addChild(title);

            auto* body = russianLabel(m_text, 14.0f, size.width - 36.0f);
            body->setAnchorPoint({0.5f, 1.0f});
            body->setPosition({size.width / 2, size.height - 48.0f});
            fitLabel(body, size.width - 36.0f, size.height - 148.0f);
            m_mainLayer->addChild(body);

            auto* menu = CCMenu::create();
            menu->setPosition({0, 0});
            m_mainLayer->addChild(menu);

            float y = 35.0f + 34.0f * (m_actions.size() - 1);
            for (std::size_t i = 0; i < m_actions.size(); ++i) {
                textButton(
                    menu, m_actions[i].first,
                    {size.width / 2, y},
                    {size.width - 38.0f, 28.0f},
                    [this, i] {
                        auto callback = m_actions[i].second;
                        this->onClose(nullptr);
                        if (callback)
                            callback();
                    }
                );
                y -= 34.0f;
            }
            return true;
        }

        void onEnter() override {
            Popup<>::onEnter();
            ++state().modalCount;
        }

        void onExit() override {
            state().modalCount = std::max(0, state().modalCount - 1);
            Popup<>::onExit();
        }

    public:
        static ChoicePopup* create(
            std::string text, std::vector<ChoiceAction> actions
        ) {
            auto area = safeRect();
            auto* popup = new ChoicePopup;
            popup->m_text = std::move(text);
            popup->m_actions = std::move(actions);

            float width = std::min(430.0f, area.size.width);
            float height = std::min(270.0f, area.size.height);

            if (!popup->initAnchored(width, height)) {
                delete popup;
                return nullptr;
            }

            auto window = CCDirector::sharedDirector()->getWinSize();
            auto delta = CCPoint{
                area.origin.x + area.size.width / 2 - window.width / 2,
                area.origin.y + area.size.height / 2 - window.height / 2
            };
            popup->m_mainLayer->setPosition(
                popup->m_mainLayer->getPosition() + delta
            );

            popup->autorelease();
            return popup;
        }
    };

    void ask(std::string text, std::vector<ChoiceAction> actions) {
        if (actions.empty())
            return;
        if (auto* popup = ChoicePopup::create(
                std::move(text), std::move(actions))) {
            popup->show();
        }
    }

    void folderRejected(std::string const& error) {
        ask(
            error +
            "\n\nНа Android выбранная папка может быть недоступна из-за "
            "системных ограничений. Гарантировать доступ на всех устройствах "
            "нельзя. Папка мода — рекомендуемый запасной вариант.",
            {
                {"Выбрать другую", [] { chooseFolder(); }},
                {"Папка мода", [] { useModFolder(); }}
            }
        );
    }

    void acceptFolder(fs::path const& parent) {
        if (parent.empty())
            return;

        fs::path directory;
        std::string error;
        if (!probeFolder(parent, directory, error)) {
            folderRejected(error);
            return;
        }

        state().directory = std::move(directory);
        Mod::get()->setSavedValue("macro-parent", pathText(parent));
        changed();
        showNotice("Папка проверена: запись разрешена.");
    }

    void useModFolder() {
        acceptFolder(Mod::get()->getSaveDir());
    }

    template <class T>
    inline constexpr bool alwaysFalse = false;

    template <class Result>
    void consumePickResult(Result&& result) {
        using T = std::remove_cvref_t<Result>;

        // VERIFY-8: фактический PickResult и представление отмены.
        // Поддержаны path, optional<path> и Result, оборачивающий один из них.
        if constexpr (std::is_same_v<T, fs::path>) {
            if (!result.empty())
                acceptFolder(result);
        }
        else if constexpr (std::is_same_v<T, std::optional<fs::path>>) {
            if (result && !result->empty())
                acceptFolder(*result);
        }
        else if constexpr (requires(T& value) {
            value.isErr();
            value.unwrap();
        }) {
            if (result.isErr()) {
                folderRejected(
                    "Выбор папки не завершён. Возможно, система отказала в доступе."
                );
            }
            else {
                consumePickResult(result.unwrap());
            }
        }
        else {
            static_assert(
                alwaysFalse<T>,
                "VERIFY-8: адаптируйте обработчик к PickResult SDK 5.11.2"
            );
        }
    }

    void chooseFolder() {
        // Нет захвата Popup/PlayLayer: callback безопасен после их закрытия.
        // callback выполняется в главном потоке согласно API Geode 5.
        async::spawn(
            file::pick(file::PickMode::OpenFolder, {}),
            [](auto result) {
                consumePickResult(std::move(result));
            }
        );
    }

    void confirmNewRecording() {
        if (!state().run.play)
            return;

        if (state().hasBuffer) {
            ask(
                "Новая запись начнётся с начала уровня и заменит макрос "
                "в буфере. Сначала сохраните его, если он нужен.",
                {
                    {"Начать новую запись", [] { requestStart(Mode::Record); }},
                    {"Сохранить буфер", [] { saveBuffer(); }}
                }
            );
        }
        else {
            requestStart(Mode::Record);
        }
    }

    void deleteFile(fs::path path) {
        ask(
            "Удалить файл «" + pathText(path.filename()) + "»?",
            {
                {"Удалить", [path = std::move(path)] {
                    std::error_code ec;
                    bool removed = fs::remove(path, ec);
                    changed();
                    showNotice(
                        removed && !ec
                            ? "Файл удалён."
                            : "Не удалось удалить файл.",
                        !removed || static_cast<bool>(ec)
                    );
                }},
                {"Отмена", [] {}}
            }
        );
    }

    class BotPopup final : public Popup<> {
        ScrollLayer* m_scroll = nullptr;
        CCLabelTTF* m_status = nullptr;
        CCLabelTTF* m_speedHint = nullptr;
        CCLabelTTF* m_tpsHint = nullptr;
        std::uint64_t m_revision = 0;

        void rebuild() {
            auto& s = state();
            auto const& r = s.run;
            auto size = m_mainLayer->getContentSize();

            if (m_scroll)
                m_scroll->removeFromParentAndCleanup(true);

            auto files = listMacros();
            float width = size.width - 24.0f;
            float viewHeight = size.height - 80.0f;

            // Geode ScrollLayer: мышь/колесо и сенсорная прокрутка.
            m_scroll = ScrollLayer::create({width, viewHeight});
            m_scroll->setPosition({12.0f, 16.0f});
            m_mainLayer->addChild(m_scroll);

            float contentHeight =
                675.0f + static_cast<float>(files.size()) * 62.0f;
            auto* content = m_scroll->m_contentLayer;
            content->setContentSize({width, contentHeight});

            auto* menu = CCMenu::create();
            menu->setPosition({0, 0});
            content->addChild(menu);

            float left = 8.0f;
            float right = width - 8.0f;
            float inner = right - left;
            float half = (inner - 8.0f) / 2.0f;
            float y = contentHeight - 22.0f;

            auto line = [&](std::string const& text, float font = 13.0f) {
                auto* label = russianLabel(text, font, inner);
                label->setAnchorPoint({0, 1});
                label->setPosition({left, y});
                content->addChild(label);
                y -= std::max(18.0f, label->getContentSize().height) + 7.0f;
                return label;
            };

            auto fullButton = [&](std::string text, std::function<void()> fn) {
                textButton(
                    menu, text, {width / 2, y - 14.0f},
                    {inner, 28.0f}, std::move(fn)
                );
                y -= 36.0f;
            };

            auto pairButtons = [&](
                std::string a, std::function<void()> fnA,
                std::string b, std::function<void()> fnB
            ) {
                textButton(
                    menu, a, {left + half / 2, y - 14.0f},
                    {half, 28.0f}, std::move(fnA)
                );
                textButton(
                    menu, b, {right - half / 2, y - 14.0f},
                    {half, 28.0f}, std::move(fnB)
                );
                y -= 36.0f;
            };

            std::string recordingText =
                r.mode == Mode::Record ? "Остановить запись" :
                (r.pendingStart == Mode::Record ? "Отменить начало записи" :
                                                 "Начать запись");

            fullButton(recordingText, [] {
                auto& runtime = state().run;
                if (runtime.mode == Mode::Record ||
                    runtime.pendingStart == Mode::Record) {
                    stopNow(false);
                }
                else {
                    confirmNewRecording();
                }
            });

            pairButtons(
                "Выключить бота", [] { stopNow(true); },
                "Запустить макрос", [] { requestStart(Mode::Playback); }
            );

            auto inputRow = [&](
                std::string caption,
                std::string value,
                std::string filter,
                std::function<void(std::string const&)> callback
            ) {
                auto* label = russianLabel(caption, 14.0f);
                label->setAnchorPoint({0, 0.5f});
                label->setPosition({left, y - 15.0f});
                fitLabel(label, inner * 0.55f, 28.0f);
                content->addChild(label);

                float inputWidth = inner * 0.39f;
                auto* input = TextInput::create(inputWidth, "", "bigFont.fnt");
                input->setFilter(filter);
                input->setString(value);
                input->setPosition({
                    right - inputWidth / 2,
                    y - 15.0f
                });
                input->setCallback(std::move(callback));
                content->addChild(input);
                y -= 39.0f;
            };

            inputRow(
                "Скорость записи",
                fmt::format("{:.2f}", s.recordingSpeed),
                "0123456789.,",
                [this](std::string const& text) {
                    double speed = 0.0;
                    if (!parseSpeed(text, speed)) {
                        if (m_speedHint)
                            m_speedHint->setString("Введите число от 0,01 до 2.");
                        return;
                    }
                    state().recordingSpeed = speed;
                    Mod::get()->setSavedValue("recording-speed", speed);
                    if (m_speedHint)
                        m_speedHint->setString("Применено. Воспроизведение — обычная скорость.");
                    syncAudio();
                }
            );
            m_speedHint = line(
                "От 0,01 до 2. Воспроизведение — обычная скорость.", 11.0f
            );

            inputRow(
                "Физических шагов/с",
                std::to_string(s.tps),
                "0123456789",
                [this](std::string const& text) {
                    std::uint32_t tps = 0;
                    if (!parseUnsigned(text, kMinTPS, kMaxTPS, tps)) {
                        if (m_tpsHint)
                            m_tpsHint->setString("Допустимо от 30 до 2000.");
                        return;
                    }
                    if (active() || state().run.pendingStart) {
                        if (m_tpsHint)
                            m_tpsHint->setString("Сначала полностью выключите бота.");
                        return;
                    }
                    state().tps = tps;
                    Mod::get()->setSavedValue(
                        "physics-tps", static_cast<int>(tps)
                    );
                    if (m_tpsHint)
                        m_tpsHint->setString("Частота изменена. Макрос должен иметь ту же частоту.");
                }
            );
            m_tpsHint = line(
                "Меняется только при полностью выключенном боте.", 11.0f
            );

            fullButton(
                std::string("Блокировать ввод: ") +
                    (s.blockInput ? "да" : "нет"),
                [] {
                    auto& current = state();
                    current.blockInput = !current.blockInput;
                    Mod::get()->setSavedValue(
                        "block-input", current.blockInput
                    );
                    if (active() && !current.run.dead)
                        sendMask(effectiveHeld());
                    changed();
                }
            );

            fullButton(
                std::string("Покадровый режим: ") +
                    (r.frameMode ? "включён" : "выключен"),
                [] { setFrameMode(!state().run.frameMode); }
            );

            inputRow(
                "Размер шага",
                std::to_string(s.frameSize),
                "0123456789",
                [](std::string const& text) {
                    std::uint32_t value = 0;
                    if (parseUnsigned(text, 1, 10000, value)) {
                        state().frameSize = value;
                        Mod::get()->setSavedValue(
                            "frame-size", static_cast<int>(value)
                        );
                    }
                }
            );

            line(
                "Назад недоступно: не подтверждено полное восстановление "
                "случайности, триггеров и анимаций. Звук в покадровом режиме "
                "приостановлен; после него может понадобиться перезапуск.",
                11.0f
            );

            inputRow(
                "Имя макроса",
                s.fileStem,
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ",
                [](std::string const& text) {
                    state().fileStem = text;
                }
            );
            line("Имя файла: латиница, цифры, пробел, дефис и подчёркивание.", 11.0f);

            pairButtons(
                "Сохранить", [] { saveBuffer(); },
                "Очистить буфер", [] {
                    ask(
                        "Удалить макрос из буфера? Файлы в папке останутся.",
                        {
                            {"Очистить", [] {
                                stopNow(true);
                                state().buffer = Macro{};
                                state().hasBuffer = false;
                                changed();
                            }},
                            {"Отмена", [] {}}
                        }
                    );
                }
            );

            line("Папка макросов", 15.0f);
            line(
                s.directory.empty()
                    ? "Папка не выбрана или недоступна."
                    : shorten(pathText(s.directory), 150),
                11.0f
            );

            pairButtons(
                "Выбрать папку", [] { chooseFolder(); },
                "Папка мода", [] { useModFolder(); }
            );

            line(
                "Выбранная папка проверяется записью тестового файла. "
                "На Android доступ зависит от системы.",
                11.0f
            );

            pairButtons(
                "Обновить список", [] { changed(); },
                "Ограничения", [] {
                    ask(
                        "Запись и запуск начинаются с начала уровня.\n"
                        "Кнопка в паузе перетаскивается мышью или пальцем.\n"
                        "Покадровый режим экспериментальный. Шаг назад отключён.\n"
                        "Локальные уровни без номера нельзя различить по заголовку.\n"
                        "Отправка результатов и прогресса не изменяется.",
                        {{"Понятно", [] {}}}
                    );
                }
            );

            line("Макросы", 16.0f);

            if (files.empty())
                line("В папке нет файлов макросов.", 12.0f);

            for (auto const& path : files) {
                auto* name = russianLabel(
                    shorten(pathText(path.filename()), 75), 12.0f
                );
                name->setAnchorPoint({0, 0.5f});
                name->setPosition({left, y - 7.0f});
                fitLabel(name, inner, 19.0f);
                content->addChild(name);

                float buttonWidth = (inner - 12.0f) / 3.0f;
                float buttonY = y - 32.0f;

                textButton(
                    menu, "Загрузить",
                    {left + buttonWidth / 2, buttonY},
                    {buttonWidth, 25.0f},
                    [path] { loadFile(path); }
                );
                textButton(
                    menu, "Играть",
                    {left + buttonWidth * 1.5f + 6.0f, buttonY},
                    {buttonWidth, 25.0f},
                    [path] {
                        if (loadFile(path))
                            requestStart(Mode::Playback);
                    }
                );
                textButton(
                    menu, "Удалить",
                    {right - buttonWidth / 2, buttonY},
                    {buttonWidth, 25.0f},
                    [path] { deleteFile(path); }
                );
                y -= 62.0f;
            }

            if (files.size() == kListLimit)
                line("Показаны первые 128 файлов.", 11.0f);

            // Если перевод/системный шрифт занял больше места, расширяем
            // содержимое без обрезания нижних строк.
            if (y < 12.0f) {
                float extra = 12.0f - y;
                auto* children = content->getChildren();
                for (unsigned i = 0; i < children->count(); ++i) {
                    auto* child = static_cast<CCNode*>(
                        children->objectAtIndex(i)
                    );
                    child->setPositionY(child->getPositionY() + extra);
                }
                content->setContentSize({width, contentHeight + extra});
            }

            m_scroll->scrollToTop();
            m_revision = s.uiRevision;
        }

    protected:
        bool setup() override {
            auto size = m_mainLayer->getContentSize();

            auto* title = russianLabel("Бот", 24.0f);
            title->setPosition({size.width / 2, size.height - 23.0f});
            m_mainLayer->addChild(title);

            m_status = russianLabel("", 11.0f);
            m_status->setPosition({size.width / 2, size.height - 48.0f});
            m_mainLayer->addChild(m_status);

            rebuild();
            scheduleUpdate();
            return true;
        }

        void onEnter() override {
            Popup<>::onEnter();
            ++state().modalCount;
        }

        void onExit() override {
            state().modalCount = std::max(0, state().modalCount - 1);
            Popup<>::onExit();
        }

        void update(float) override {
            auto const& s = state();
            auto const& r = s.run;

            if (m_revision != s.uiRevision)
                rebuild();

            std::string mode =
                r.pendingStart ? "Ожидание выхода из паузы" :
                r.mode == Mode::Record ? "Запись" :
                r.mode == Mode::Playback ? "Воспроизведение" :
                r.frameMode ? "Покадровый режим" : "Выключен";

            auto text = fmt::format(
                "{} · шаг {} · действий {}",
                mode, r.step, s.hasBuffer ? s.buffer.actions.size() : 0
            );
            m_status->setString(text.c_str());
            fitLabel(
                m_status, m_mainLayer->getContentSize().width - 40.0f, 18.0f
            );
        }

    public:
        static BotPopup* create() {
            auto area = safeRect();
            auto* popup = new BotPopup;

            float width = std::min(500.0f, area.size.width);
            float height = std::min(360.0f, area.size.height);

            if (!popup->initAnchored(width, height)) {
                delete popup;
                return nullptr;
            }

            auto window = CCDirector::sharedDirector()->getWinSize();
            popup->m_mainLayer->setPosition(
                popup->m_mainLayer->getPosition() + CCPoint{
                    area.origin.x + area.size.width / 2 - window.width / 2,
                    area.origin.y + area.size.height / 2 - window.height / 2
                }
            );

            popup->autorelease();
            return popup;
        }

        void animateOpen() {
            m_mainLayer->setScale(0.92f);
            m_mainLayer->runAction(
                CCEaseBackOut::create(CCScaleTo::create(0.18f, 1.0f))
            );
        }
    };

    void openPanel() {
        ensureConfiguration();
        auto const& r = state().run;
        if (!r.play || !r.play->m_isPaused) {
            showNotice("Панель открывается только из меню паузы.", true);
            return;
        }

        if (auto* popup = BotPopup::create()) {
            popup->show();
            popup->animateOpen();
        }
    }

    class DraggableLauncher final : public CCLayer {
        CCNode* m_box = nullptr;
        CCPoint m_start;
        CCPoint m_offset;
        bool m_dragged = false;
        int m_touchID = -1;

        CCPoint clampPosition(CCPoint position) const {
            auto area = safeRect();
            constexpr float halfWidth = 27.0f;
            constexpr float halfHeight = 16.0f;

            position.x = std::clamp(
                position.x,
                area.origin.x + halfWidth,
                area.origin.x + area.size.width - halfWidth
            );
            position.y = std::clamp(
                position.y,
                area.origin.y + halfHeight,
                area.origin.y + area.size.height - halfHeight
            );
            return position;
        }

        void savePosition() {
            auto area = safeRect();
            auto position = m_box->getPosition();

            double x = (position.x - area.origin.x) / area.size.width;
            double y = (position.y - area.origin.y) / area.size.height;

            Mod::get()->setSavedValue("launcher-x", std::clamp(x, 0.0, 1.0));
            Mod::get()->setSavedValue("launcher-y", std::clamp(y, 0.0, 1.0));
        }

    public:
        static DraggableLauncher* create() {
            auto* layer = new DraggableLauncher;
            if (!layer->init()) {
                delete layer;
                return nullptr;
            }

            layer->setTouchEnabled(true);
            layer->setContentSize(
                CCDirector::sharedDirector()->getWinSize()
            );

            layer->m_box = CCNode::create();
            layer->m_box->setContentSize({54.0f, 32.0f});
            layer->m_box->setAnchorPoint({0.5f, 0.5f});
            layer->addChild(layer->m_box);

            auto* background = CCLayerColor::create(
                {38, 78, 120, 245}, 54.0f, 32.0f
            );
            layer->m_box->addChild(background);

            auto* label = russianLabel("Бот", 16.0f);
            label->setPosition({27.0f, 16.0f});
            layer->m_box->addChild(label);

            auto area = safeRect();
            double x = Mod::get()->getSavedValue<double>("launcher-x", 0.0);
            double y = Mod::get()->getSavedValue<double>("launcher-y", 0.5);
            if (!std::isfinite(x)) x = 0.0;
            if (!std::isfinite(y)) y = 0.5;

            layer->m_box->setPosition(layer->clampPosition({
                area.origin.x + static_cast<float>(std::clamp(x, 0.0, 1.0)) *
                                    area.size.width,
                area.origin.y + static_cast<float>(std::clamp(y, 0.0, 1.0)) *
                                    area.size.height
            }));

            layer->autorelease();
            return layer;
        }

        void registerWithTouchDispatcher() override {
            CCDirector::sharedDirector()->getTouchDispatcher()
                ->addTargetedDelegate(this, -600, true);
        }

        bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
            auto const& s = state();
            if (s.modalCount > 0 || !s.run.play ||
                !s.run.play->m_isPaused || m_touchID != -1) {
                return false;
            }

            auto point = convertToNodeSpace(touch->getLocation());
            if (!m_box->boundingBox().containsPoint(point))
                return false;

            m_touchID = touch->getID();
            m_start = point;
            m_offset = point - m_box->getPosition();
            m_dragged = false;
            return true;
        }

        void ccTouchMoved(CCTouch* touch, CCEvent*) override {
            if (touch->getID() != m_touchID)
                return;

            auto point = convertToNodeSpace(touch->getLocation());
            if (std::hypot(point.x - m_start.x, point.y - m_start.y) > 6.0f)
                m_dragged = true;

            if (m_dragged)
                m_box->setPosition(clampPosition(point - m_offset));
        }

        void ccTouchEnded(CCTouch* touch, CCEvent*) override {
            if (touch->getID() != m_touchID)
                return;

            m_touchID = -1;
            if (m_dragged)
                savePosition();
            else
                openPanel();
        }

        void ccTouchCancelled(CCTouch*, CCEvent*) override {
            if (m_dragged)
                savePosition();
            m_touchID = -1;
        }
    };

    // -----------------------------------------------------------------
    // ПАТЧ 4 / двойной вызов pumpUI().
    //
    // Было: FrameControls — отдельный CCLayer со своим scheduleUpdate(),
    // который тикает независимо от хука GJBaseGameLayer::update. Его
    // update() дополнительно вызывал pumpUI(), хотя тот и так вызывается
    // безусловно (вне зависимости от controls(this)) каждый кадр из
    // GDBotBaseLayer::update ниже по файлу. Сейчас это не создавало
    // видимого бага только потому, что pumpUI() идемпотентна в пределах
    // кадра, но любое будущее изменение pumpUI(), которое начнёт
    // накапливать состояние между вызовами, тихо сломалось бы из-за этого
    // скрытого повторного вызова.
    //
    // Стало: pumpUI() вызывается только из одного места — из хука
    // GJBaseGameLayer::update. FrameControls::update() отвечает только
    // за собственную видимость и состояние своих кнопок.
    // -----------------------------------------------------------------

    class FrameControls final : public CCLayer {
        PlayLayer* m_owner = nullptr;
        CCMenu* m_menu = nullptr;
        CCLabelTTF* m_hint = nullptr;

    public:
        static FrameControls* create(PlayLayer* owner) {
            auto* layer = new FrameControls;
            if (!layer->init()) {
                delete layer;
                return nullptr;
            }

            layer->m_owner = owner;
            layer->m_menu = CCMenu::create();
            layer->m_menu->setPosition({0, 0});
            layer->addChild(layer->m_menu);

            auto area = safeRect();
            float x = area.origin.x + 46.0f;
            float y = area.origin.y + area.size.height / 2;

            textButton(
                layer->m_menu, "Вправо",
                {x, y + 22.0f}, {88.0f, 30.0f},
                [] { advanceFrames(); }
            );

            textButton(
                layer->m_menu, "Влево",
                {x, y - 16.0f}, {88.0f, 30.0f},
                [] {}, false
            );

            layer->m_hint = russianLabel(
                "Назад недоступно", 10.0f
            );
            layer->m_hint->setPosition({x, y - 42.0f});
            layer->addChild(layer->m_hint);

            layer->scheduleUpdate();
            layer->setVisible(false);
            layer->m_menu->setEnabled(false);
            layer->autorelease();
            return layer;
        }

        void update(float) override {
            auto const& r = state().run;
            bool visible = r.play == m_owner &&
                           r.frameMode &&
                           !m_owner->m_isPaused;

            setVisible(visible);
            m_menu->setEnabled(visible && !r.dead);
            // pumpUI() намеренно НЕ вызывается здесь — см. ПАТЧ 4 выше.
        }
    };

    void pumpUI() {
        auto& s = state();

        if (!s.notices.empty()) {
            auto messages = std::move(s.notices);
            s.notices.clear();
            for (auto const& notice : messages)
                showNotice(notice.text, notice.warning);
        }

        if (s.deathQuestion && s.modalCount == 0) {
            s.deathQuestion = false;
            auto session = s.deathQuestionSession;

            ask(
                "Запись остановлена, макрос остался в буфере.\n"
                "Его можно сохранить или начать новую запись с начала уровня.",
                {
                    {"Сохранить буфер", [] { saveBuffer(); }},
                    {"Новая запись", [session] {
                        if (state().run.play &&
                            state().run.session == session) {
                            requestStart(Mode::Record);
                        }
                    }}
                }
            );
        }
    }

    // VERIFY-11:
    // takeStateSnapshot()/compareStateSnapshot() здесь НЕ вызываются.
    // Их наличие не доказывает возможность сохранить/восстановить полный RNG,
    // триггеры, действия Cocos, аудио и состояние сторонних модов.
    // Поэтому кнопка «Влево» действительно выключена, а не имитирует перемотку.
}

// -------------------------------------------------------------------------
// Хуки. Деструкторы в $modify не переопределяются.
// -------------------------------------------------------------------------

class $modify(GDBotBaseLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        if (!self.setHookPriorityPre(
                "GJBaseGameLayer::queueButton", Priority::First)) {
            log::warn("Не удалось установить первый приоритет очереди ввода.");
        }

        if (!self.setHookPriorityPost(
                "GJBaseGameLayer::getModifiedDelta", Priority::Last)) {
            log::warn("Не удалось установить последний приоритет шага физики.");
        }

        if (!self.setHookPriorityPre(
                "GJBaseGameLayer::processCommands", Priority::First)) {
            log::warn("Не удалось установить приоритет собственных подшагов.");
        }

        if (!self.setHookPriorityPre(
                "GJBaseGameLayer::processQueuedButtons", Priority::First)) {
            log::warn("Не удалось установить приоритет обработки очереди.");
        }

        if (!self.setHookPriorityPre(
                "GJBaseGameLayer::handleButton", Priority::First)) {
            log::warn("Не удалось установить приоритет прямого ввода.");
        }

        if (!self.setHookPriorityPre(
                "GJBaseGameLayer::shouldUseSubstepForButton", Priority::First)) {
            log::warn("Не удалось установить приоритет нативных подшагов.");
        }
    }

    void queueButton(
        int button, bool push, bool isPlayer2, double timestamp
    ) {
        if (!gdbot::controls(this)) {
            GJBaseGameLayer::queueButton(
                button, push, isPlayer2, timestamp
            );
            return;
        }

        auto const& r = gdbot::state().run;
        if (!r.injecting)
            gdbot::enqueueRaw(push, button, isPlayer2, timestamp);

        // Намеренно НЕ вызываем оригинал.
        // Следующие хуки queueButton, в том числе внешнего CBF, не исполняются.
    }

    // ПАТЧ 3 / недокументированное допущение.
    //
    // Хук handleButton — это НЕ страховка "на всякий случай", а отдельный
    // источник ввода. На практике на Android (а возможно и в части десктопных
    // путей) часть элементов управления — экранные кнопки, геймпад — могут
    // вызывать GJBaseGameLayer::handleButton НАПРЯМУЮ, минуя queueButton и
    // связанный с ним timestamp из системы ввода. Если бы это предположение
    // было неверным и handleButton никогда не вызывался в обход очереди,
    // перехват ниже был бы избыточен, но безвреден; если оно верно (похоже
    // на правду по доступным описаниям 2.2081/Geode), то без этого хука
    // бот вообще не увидел бы часть сенсорного ввода на телефоне.
    //
    // VERIFY-12: подтвердить на реальном устройстве Android, что (а)
    // экранные кнопки действительно идут через handleButton в обход
    // queueButton, и (б) что внутри processCommands/checkCollisions движок
    // не вызывает handleButton для "виртуальных" причин (отпускание при
    // касании орба и т.п.) вне контекста r.inSlice — иначе такие вызовы
    // будут ошибочно считаны как пользовательский ввод следующего кадра.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!gdbot::controls(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& r = gdbot::state().run;

        if (r.injecting || r.resetDepth || r.loadDepth) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (r.pauseGuard || r.paused ||
            (r.play && r.play->m_isPaused) || r.dead) {
            return;
        }

        // Внутренние вызовы движка из физического ядра не превращаются
        // в новые пользовательские события следующего кадра.
        if (r.inSlice) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        gdbot::enqueueRaw(
            down, button, !isPlayer1, gdbot::inputTimestamp()
        );
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        if (gdbot::controls(this)) {
            this->m_queuedButtons.clear();
            return;
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }

    bool shouldUseSubstepForButton(float dt) {
        if (gdbot::controls(this))
            return false;
        return GJBaseGameLayer::shouldUseSubstepForButton(dt);
    }

    double getModifiedDelta(float dt) {
        // Оригинал обязательно вызывается первым: он меняет состояние слоя.
        double native = GJBaseGameLayer::getModifiedDelta(dt);

        if (!gdbot::controls(this))
            return native;

        // VERIFY-1: проверить фактический результат цепочки Post/Last
        // на 2.2081 вместе с TPS bypass, speedhack и внешним CBF.
        return 1.0 / static_cast<double>(gdbot::state().tps);
    }

    // VERIFY-1: наличие/хук GJBaseGameLayer::update(float) в нужных bindings.
    void update(float dt) {
        gdbot::driveUpdate(this, dt, [this](float fixedDelta) {
            GJBaseGameLayer::update(fixedDelta);
        });
        // Единственное место вызова pumpUI(): выполняется каждый кадр,
        // независимо от того, контролирует ли бот именно этот слой —
        // см. ПАТЧ 4.
        gdbot::pumpUI();
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        gdbot::splitCommands(
            this, dt, isHalfTick, isLastTick,
            [this](float slice, bool half, bool last) {
                GJBaseGameLayer::processCommands(slice, half, last);
            }
        );
    }

    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        auto& r = gdbot::state().run;
        if (gdbot::controls(this) && r.inTick && !r.inSlice &&
            !r.resetDepth && !r.loadDepth) {
            // Это детектор, а не барьер (см. примечание в начале файла):
            // к моменту попадания сюда нативный код уже выполнил физику с
            // делением на подшаги, которое наш CBF не учёл. Остановить
            // уже исполняющийся нативный вызов отсюда нельзя — можно только
            // пометить шаг как недостоверный, чтобы driveUpdate() обрезал
            // его из записи и безопасно остановил бота сразу после возврата.
            r.badKernel = true;
        }

        return GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
    }
};

class $modify(GDBotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;

        gdbot::attach(this, level);

        if (auto* controls = gdbot::FrameControls::create(this))
            this->addChild(controls, 10000);

        return true;
    }

    CheckpointObject* createCheckpoint() {
        auto* checkpoint = PlayLayer::createCheckpoint();

        if (checkpoint && gdbot::owns(this) && gdbot::active()) {
            auto& r = gdbot::state().run;
            if (r.checkpoints.size() > 4096)
                gdbot::pruneCheckpoints();
            r.checkpoints[checkpoint] = gdbot::captureCheckpointData();
        }
        return checkpoint;
    }

    CheckpointObject* markCheckpoint() {
        auto& r = gdbot::state().run;
        if (gdbot::owns(this) && gdbot::active() &&
            r.inTick && !r.commandsDone && !r.drainingMark) {
            r.markPending = true;

            // VERIFY-4:
            // нативные вызывающие markCheckpoint должны допускать nullptr.
            // Запрос исполняется на ближайшей целой границе шага.
            return nullptr;
        }

        return PlayLayer::markCheckpoint();
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);

        if (gdbot::owns(this) && gdbot::active()) {
            auto& r = gdbot::state().run;
            if (checkpoint && !r.checkpoints.contains(checkpoint))
                r.checkpoints[checkpoint] = gdbot::captureCheckpointData();
            gdbot::pruneCheckpoints();
        }
    }

    void removeCheckpoint(bool first) {
        PlayLayer::removeCheckpoint(first);
        if (gdbot::owns(this))
            gdbot::pruneCheckpoints();
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        auto& r = gdbot::state().run;
        bool tracked = gdbot::owns(this);
        auto session = r.session;
        std::optional<gdbot::CheckpointData> data;

        if (tracked) {
            auto found = r.checkpoints.find(checkpoint);
            if (found != r.checkpoints.end())
                data = found->second;
            ++r.loadDepth;
        }

        PlayLayer::loadFromCheckpoint(checkpoint);

        if (!tracked || !gdbot::owns(this) || r.session != session)
            return;

        --r.loadDepth;

        if (r.resetDepth > 0) {
            r.loadedDuringReset = true;
            r.restoreAfterReset = data;
        }
        else if (gdbot::active()) {
            gdbot::restoreCheckpointData(data);
            if (!r.inTick)
                gdbot::flushStop();
        }
    }

    void resetLevel() {
        bool tracked = gdbot::owns(this);
        auto session = gdbot::state().run.session;
        if (tracked)
            gdbot::resetBegin();

        PlayLayer::resetLevel();

        if (tracked && gdbot::owns(this) &&
            gdbot::state().run.session == session) {
            gdbot::resetEnd();
        }
    }

    void resetLevelFromStart() {
        bool tracked = gdbot::owns(this);
        auto session = gdbot::state().run.session;
        if (tracked)
            gdbot::resetBegin();

        PlayLayer::resetLevelFromStart();

        if (tracked && gdbot::owns(this) &&
            gdbot::state().run.session == session) {
            gdbot::resetEnd();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);

        // Не считаем смертью вызов, который нативная игра фактически отклонила.
        if (gdbot::owns(this) && this->m_playerDied)
            gdbot::onDeath();
    }

    void pauseGame(bool value) {
        bool tracked = gdbot::owns(this);
        auto session = gdbot::state().run.session;

        if (tracked) {
            auto& r = gdbot::state().run;
            r.pauseGuard = true;
            r.paused = true;
            gdbot::restoreAudio();
        }

        // VERIFY-5: нативный сброс кнопок при паузе должен проходить через
        // перехватываемый handleButton либо быть безопасно восстановимым.
        PlayLayer::pauseGame(value);

        if (tracked && gdbot::owns(this) &&
            gdbot::state().run.session == session) {
            auto& r = gdbot::state().run;
            r.pauseGuard = false;
            r.paused = this->m_isPaused;
        }
    }

    void levelComplete() {
        // Отправка прогресса и результатов остается полностью нативной.
        PlayLayer::levelComplete();

        if (gdbot::owns(this) && gdbot::active()) {
            gdbot::requestStop("Уровень завершён. Макрос оставлен в буфере.");
            gdbot::restoreAudio();
            if (!gdbot::state().run.inTick)
                gdbot::flushStop();
        }
    }

    void onQuit() {
        // Все обращения к живому PlayLayer — ДО оригинального onQuit.
        gdbot::detach(this);
        PlayLayer::onQuit();
    }
};

class $modify(GDBotPauseLayer, PauseLayer) {
    // VERIFY-10: virtual void PauseLayer::customSetup() в bindings 2.2081.
    void customSetup() {
        PauseLayer::customSetup();

        if (!gdbot::state().run.play)
            return;

        gdbot::ensureConfiguration();
        if (auto* launcher = gdbot::DraggableLauncher::create())
            this->addChild(launcher, 10000);
    }
};
