#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace bot {
    struct Action {
        int step;
        int button;
        bool push;
        bool player2;
        double frac; // [0.0 .. 1.0] позиция внутри шага
    };

    struct SubstepAction {
        int button;          // 0 = нет события (просто симуляция времени), иначе PlayerButton
        bool push;
        bool isPlayer2;
        double deltaFactor;  // доля шага [0.0 .. 1.0]
    };

    enum class Mode {
        Idle,
        Record,
        Play
    };

    inline Mode mode = Mode::Idle;
    inline std::vector<Action> macro;
    inline int currentStep = 0;

    inline double lastFrameTime = 0.0;
    inline double currentFrameTime = 0.0;

    inline std::vector<PlayerButtonCommand> frameCommands;
    inline std::vector<SubstepAction> p1Split;
    inline std::vector<SubstepAction> p2Split;

    inline double nowSeconds() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    // Забираем сырые кнопки из очереди движка без каких-либо искажений
    inline void drainQueuedButtons(GJBaseGameLayer* layer) {
        if (!layer) return;
        auto& queued = layer->m_queuedButtons;
        if (queued.empty()) return;

        for (const auto& cmd : queued) {
            frameCommands.push_back(cmd);
        }
        queued.clear();
    }

    // Честное разбиение физического шага на суб-тики
    inline std::vector<SubstepAction> buildStepQueue(
        const std::vector<PlayerButtonCommand>& rawCommands,
        bool isPlayer2,
        int step,
        double stepStart,
        double stepEnd
    ) {
        std::vector<SubstepAction> result;
        double stepDur = stepEnd - stepStart;
        if (stepDur <= 0.0) return result;

        struct StepEvent {
            int button;
            bool push;
            double t;
        };

        std::vector<StepEvent> stepEvents;
        for (const auto& cmd : rawCommands) {
            if (cmd.m_isPlayer2 != isPlayer2) continue;

            // Проверяем принадлежность шагу с защитой от микро-джиттера таймера
            bool inStep = false;
            if (cmd.m_timestamp >= stepStart && cmd.m_timestamp < stepEnd) {
                inStep = true;
            } else if (step == 0 && cmd.m_timestamp < stepStart) {
                inStep = true;
            }

            if (inStep) {
                double t = (cmd.m_timestamp - stepStart) / stepDur;
                t = std::clamp(t, 0.0, 1.0);
                stepEvents.push_back({cmd.m_button, cmd.m_isPush, t});

                if (bot::mode == bot::Mode::Record) {
                    bot::macro.push_back({bot::currentStep, cmd.m_button, cmd.m_isPush, isPlayer2, t});
                }
            }
        }

        if (stepEvents.empty()) {
            return result;
        }

        // Сортируем события по времени возникновения внутри тика
        std::sort(stepEvents.begin(), stepEvents.end(), [](const StepEvent& a, const StepEvent& b) {
            return a.t < b.t;
        });

        // 1. Отрезок ДО первого клика: крутим физику с исходным состоянием
        if (stepEvents[0].t > 0.00001) {
            result.push_back({0, false, isPlayer2, stepEvents[0].t});
        }

        // 2. Отрезки между кликами и до конца шага
        for (size_t i = 0; i < stepEvents.size(); ++i) {
            double nextT = (i + 1 < stepEvents.size()) ? stepEvents[i + 1].t : 1.0;
            double slice = nextT - stepEvents[i].t;
            if (slice > 0.00001) {
                result.push_back({stepEvents[i].button, stepEvents[i].push, isPlayer2, slice});
            } else {
                // Если два клика произошли в одну микросекунду (быстрый спам),
                // состояние обязательно переключаем без потери времени
                result.push_back({stepEvents[i].button, stepEvents[i].push, isPlayer2, 0.0});
            }
        }

        return result;
    }

    inline void onFrameStart() {
        double now = nowSeconds();
        if (lastFrameTime <= 0.0) {
            lastFrameTime = now - (1.0 / 60.0);
        } else {
            lastFrameTime = currentFrameTime;
        }
        currentFrameTime = now;

        frameCommands.clear();
        p1Split.clear();
        p2Split.clear();
    }

    inline void resetEngine() {
        currentStep = 0;
        lastFrameTime = 0.0;
        currentFrameTime = 0.0;
        frameCommands.clear();
        p1Split.clear();
        p2Split.clear();
    }
}

// ---------------------------------------------------------------------
// CCScheduler Hook
// ---------------------------------------------------------------------
struct BotScheduler : Modify<BotScheduler, CCScheduler> {
    void update(float dt) {
        bot::onFrameStart();
        CCScheduler::update(dt);
    }
};

// ---------------------------------------------------------------------
// GJBaseGameLayer Hook (перехват ввода)
// ---------------------------------------------------------------------
struct BotBaseGameLayer : Modify<BotBaseGameLayer, GJBaseGameLayer> {
    void update(float dt) {
        if (bot::mode == bot::Mode::Record) {
            bot::drainQueuedButtons(this);
        }
        GJBaseGameLayer::update(dt);
    }
};

// ---------------------------------------------------------------------
// PlayerObject Hook (выполнение физических суб-шагов)
// ---------------------------------------------------------------------
struct BotPlayerObject : Modify<BotPlayerObject, PlayerObject> {
    void update(float dt) {
        auto* playLayer = PlayLayer::get();
        if (!playLayer) {
            PlayerObject::update(dt);
            return;
        }

        bool isP1 = (this == playLayer->m_player1);
        bool isP2 = (this == playLayer->m_player2);

        if (!isP1 && !isP2) {
            PlayerObject::update(dt);
            return;
        }

        // РЕЖИМ ЗАПИСИ
        if (bot::mode == bot::Mode::Record) {
            if (isP1) {
                bot::p1Split = bot::buildStepQueue(bot::frameCommands, false, 0, bot::lastFrameTime, bot::currentFrameTime);
                bot::p2Split = bot::buildStepQueue(bot::frameCommands, true,  0, bot::lastFrameTime, bot::currentFrameTime);
            }

            auto& split = isP1 ? bot::p1Split : bot::p2Split;
            if (!split.empty()) {
                for (const auto& act : split) {
                    if (act.button != 0) {
                        if (act.push) {
                            PlayerObject::pushButton(static_cast<PlayerButton>(act.button));
                        } else {
                            PlayerObject::releaseButton(static_cast<PlayerButton>(act.button));
                        }
                    }
                    if (act.deltaFactor > 0.0) {
                        PlayerObject::update(static_cast<float>(dt * act.deltaFactor));
                    }
                }
            } else {
                PlayerObject::update(dt);
            }

            if (isP1) {
                bot::currentStep++;
            }
            return;
        }

        // РЕЖИМ ВОСПРОИЗВЕДЕНИЯ (PLAY)
        if (bot::mode == bot::Mode::Play) {
            struct PlayEvent {
                int button;
                bool push;
                double frac;
            };
            std::vector<PlayEvent> stepEvents;

            for (const auto& act : bot::macro) {
                if (act.step == bot::currentStep && act.player2 == isP2) {
                    stepEvents.push_back({act.button, act.push, act.frac});
                }
            }

            if (!stepEvents.empty()) {
                std::sort(stepEvents.begin(), stepEvents.end(), [](const PlayEvent& a, const PlayEvent& b) {
                    return a.frac < b.frac;
                });

                // Время до первого действия
                if (stepEvents[0].frac > 0.00001) {
                    PlayerObject::update(static_cast<float>(dt * stepEvents[0].frac));
                }

                // Шаги между действиями
                for (size_t i = 0; i < stepEvents.size(); ++i) {
                    if (stepEvents[i].push) {
                        PlayerObject::pushButton(static_cast<PlayerButton>(stepEvents[i].button));
                    } else {
                        PlayerObject::releaseButton(static_cast<PlayerButton>(stepEvents[i].button));
                    }

                    double nextT = (i + 1 < stepEvents.size()) ? stepEvents[i + 1].frac : 1.0;
                    double slice = nextT - stepEvents[i].frac;
                    if (slice > 0.00001) {
                        PlayerObject::update(static_cast<float>(dt * slice));
                    }
                }
            } else {
                PlayerObject::update(dt);
            }

            if (isP1) {
                bot::currentStep++;
            }
            return;
        }

        // Обычная игра без мода
        PlayerObject::update(dt);
    }
};

// ---------------------------------------------------------------------
// PlayLayer Hook
// ---------------------------------------------------------------------
struct BotPlayLayer : Modify<BotPlayLayer, PlayLayer> {
    void resetLevel() {
        PlayLayer::resetLevel();
        bot::resetEngine();
    }

    void onQuit() {
        bot::resetEngine();
        bot::mode = bot::Mode::Idle;
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------
// PauseLayer Hook (UI меню мода)
// ---------------------------------------------------------------------
struct BotPauseLayer : Modify<BotPauseLayer, PauseLayer> {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();
        menu->setPosition({60.f, 120.f});
        menu->setID("tas-bot-menu"_spr);

        auto recLabel = CCLabelBMFont::create("REC", "bigFont.fnt");
        recLabel->setScale(0.5f);
        auto recBtn = CCMenuItemSpriteExtra::create(
            recLabel, this, menu_selector(BotPauseLayer::onToggleRecord)
        );

        auto playLabel = CCLabelBMFont::create("PLAY", "bigFont.fnt");
        playLabel->setScale(0.5f);
        auto playBtn = CCMenuItemSpriteExtra::create(
            playLabel, this, menu_selector(BotPauseLayer::onTogglePlay)
        );

        auto clearLabel = CCLabelBMFont::create("CLEAR", "bigFont.fnt");
        clearLabel->setScale(0.5f);
        auto clearBtn = CCMenuItemSpriteExtra::create(
            clearLabel, this, menu_selector(BotPauseLayer::onClearMacro)
        );

        menu->addChild(recBtn);
        menu->addChild(playBtn);
        menu->addChild(clearBtn);
        menu->alignItemsVerticallyWithPadding(10.f);

        this->addChild(menu);
    }

    void onToggleRecord(CCObject*) {
        if (bot::mode == bot::Mode::Record) {
            bot::mode = bot::Mode::Idle;
            FLAlertLayer::create("TAS Bot", "Recording <cr>stopped</c>.", "OK")->show();
        } else {
            bot::mode = bot::Mode::Record;
            bot::macro.clear();
            bot::resetEngine();
            FLAlertLayer::create("TAS Bot", "Recording <cg>started</c>.", "OK")->show();
        }
    }

    void onTogglePlay(CCObject*) {
        if (bot::mode == bot::Mode::Play) {
            bot::mode = bot::Mode::Idle;
            FLAlertLayer::create("TAS Bot", "Playback <cr>stopped</c>.", "OK")->show();
        } else {
            bot::mode = bot::Mode::Play;
            bot::resetEngine();
            FLAlertLayer::create("TAS Bot", "Playback <cg>started</c>.", "OK")->show();
        }
    }

    void onClearMacro(CCObject*) {
        bot::macro.clear();
        bot::resetEngine();
        bot::mode = bot::Mode::Idle;
        FLAlertLayer::create("TAS Bot", "Macro cleared.", "OK")->show();
    }
};
