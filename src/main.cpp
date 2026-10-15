#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>

#include <Geode/layout/RowLayout.hpp>

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdint>
#include <string>

using namespace geode::prelude;

enum class BotMode {
    Disabled,
    Record,
    Play
};

struct BotAction {
    double time;
    bool push;
    int button;
    bool player1;
};

struct PressedKey {
    int button;
    bool player1;
};

struct CheckpointSnapshot {
    double time;
    size_t actionCount;
    std::vector<PressedKey> pressed;
};

// --------------------------------------------------
// Global bot state
// --------------------------------------------------

static BotMode s_mode = BotMode::Disabled;

static double s_time = 0.0;
static size_t s_actionIndex = 0;

static std::vector<BotAction> s_actions;

static std::unordered_map<CheckpointObject*, CheckpointSnapshot> s_snapshots;
static std::vector<CheckpointObject*> s_checkpointStack;

// Currently pressed buttons.
static std::unordered_set<uint64_t> s_pressed;

// Synthetic input depth: prevents bot's own button presses from being recorded.
static int s_scriptDepth = 0;

// Protects checkpoint state while loadFromCheckpoint() is running.
static int s_loadDepth = 0;

// Deferred actions processed inside PlayLayer::update().
static bool s_pendingRestart = false;
static bool s_pendingRelease = false;

// Blocks real player input while level restart is pending.
static bool s_blockRealInput = false;

// Set when replay state was changed during button processing.
static bool s_stateChanged = false;

// Current PauseLayer node, used to refresh separate buttons.
static CCNode* s_pauseNode = nullptr;

// --------------------------------------------------
// Forward declarations
// --------------------------------------------------

static void refreshBotUI();

// --------------------------------------------------
// Helpers
// --------------------------------------------------

static uint64_t makeButtonKey(int button, bool player1) {
    return
        (static_cast<uint64_t>(static_cast<uint32_t>(button)) << 1) |
        (player1 ? 0ULL : 1ULL);
}

static PressedKey decodeButtonKey(uint64_t key) {
    return {
        static_cast<int>(static_cast<uint32_t>(key >> 1)),
        (key & 1ULL) == 0ULL
    };
}

static std::vector<PressedKey> getCurrentPressedKeys() {
    std::vector<PressedKey> out;
    out.reserve(s_pressed.size());

    for (auto key : s_pressed) {
        out.push_back(decodeButtonKey(key));
    }

    return out;
}

static void clearCheckpointState() {
    s_snapshots.clear();
    s_checkpointStack.clear();
}

static void truncateCheckpointsAfter(CheckpointObject* cp) {
    if (!cp) {
        return;
    }

    auto it = std::find(s_checkpointStack.begin(), s_checkpointStack.end(), cp);
    if (it == s_checkpointStack.end()) {
        return;
    }

    // Remove snapshots after the loaded checkpoint.
    for (auto x = it + 1; x != s_checkpointStack.end(); ++x) {
        s_snapshots.erase(*x);
    }

    s_checkpointStack.erase(it + 1, s_checkpointStack.end());
}

static void refreshBotUI() {
    if (!s_pauseNode) {
        return;
    }

    auto menuNode = s_pauseNode->getChildByID("bot-control-menu");
    auto menu = typeinfo_cast<CCMenu*>(menuNode);

    if (!menu) {
        return;
    }

    auto setButton = [&](char const* id, char const* text, bool active) {
        auto itemNode = menu->getChildByID(id);
        auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(itemNode);

        if (!item) {
            return;
        }

        auto spr = ButtonSprite::create(text);
        if (spr) {
            spr->setScale(0.7f);
            item->setNormalImage(spr);
        }

        item->setOpacity(active ? 255 : 150);
    };

    setButton("bot-off-btn", "OFF", s_mode == BotMode::Disabled);
    setButton("bot-rec-btn", "REC", s_mode == BotMode::Record);
    setButton("bot-play-btn", "PLAY", s_mode == BotMode::Play);
    setButton("bot-clear-btn", "CLEAR", false);
}

static void setBotMode(BotMode next, bool restart, bool clearMacro) {
    if (clearMacro) {
        s_actions.clear();
    }

    s_mode = next;
    s_time = 0.0;
    s_actionIndex = 0;

    clearCheckpointState();

    if (restart) {
        s_pendingRestart = true;
        s_pendingRelease = false;
        s_blockRealInput = true;
    } else {
        s_pendingRestart = false;
        s_pendingRelease = true;
        s_blockRealInput = false;
    }

    s_stateChanged = true;
    refreshBotUI();
}

// --------------------------------------------------
// 1. Button input hook
// --------------------------------------------------

class $modify(BotBaseGameLayer, GJBaseGameLayer) {
public:
    void handleButton(bool push, int button, bool player1) {
        uint64_t key = makeButtonKey(button, player1);

        // Synthetic/scripted input:
        // - do not record it;
        // - allow it even in Play mode;
        // - update pressed state.
        if (s_scriptDepth > 0) {
            if (push) {
                s_pressed.insert(key);
            } else {
                s_pressed.erase(key);
            }

            GJBaseGameLayer::handleButton(push, button, player1);
            return;
        }

        // Ignore real input while restart is pending.
        if (s_blockRealInput) {
            return;
        }

        // In Play mode human input is ignored.
        if (s_mode == BotMode::Play) {
            return;
        }

        // In Record mode store real input events.
        if (s_mode == BotMode::Record) {
            s_actions.push_back({ s_time, push, button, player1 });
        }

        // Track pressed state for real input.
        if (push) {
            s_pressed.insert(key);
        } else {
            s_pressed.erase(key);
        }

        GJBaseGameLayer::handleButton(push, button, player1);
    }
};

// --------------------------------------------------
// 2. PlayLayer hooks: timing, checkpoints, restart
// --------------------------------------------------

class $modify(BotPlayLayer, PlayLayer) {
public:
    void releaseAllButtons() {
        auto pressed = getCurrentPressedKeys();

        ++s_scriptDepth;
        for (auto const& p : pressed) {
            this->handleButton(false, p.button, p.player1);
        }
        --s_scriptDepth;

        s_pressed.clear();
    }

    void applyButtons(std::vector<PressedKey> const& pressed) {
        ++s_scriptDepth;
        for (auto const& p : pressed) {
            this->handleButton(true, p.button, p.player1);
        }
        --s_scriptDepth;
    }

    void update(float dt) {
        // Apply pending restart from PauseLayer.
        if (s_pendingRestart) {
            s_pendingRestart = false;
            s_pendingRelease = false;

            // Release buttons before resetting level.
            this->releaseAllButtons();
            this->resetLevel();

            s_blockRealInput = false;
            s_stateChanged = false;
        }

        // Apply pending release, e.g. when Bot is turned OFF.
        if (s_pendingRelease) {
            s_pendingRelease = false;
            this->releaseAllButtons();
            s_stateChanged = false;
        }

        // Safety: if Play mode was entered with empty macro, disable bot.
        if (s_mode == BotMode::Play && s_actions.empty()) {
            setBotMode(BotMode::Disabled, false, false);
        }

        if (s_mode != BotMode::Disabled) {
            s_time += static_cast<double>(dt);

            if (s_mode == BotMode::Play) {
                while (
                    s_actionIndex < s_actions.size() &&
                    s_time >= s_actions[s_actionIndex].time
                ) {
                    BotAction act = s_actions[s_actionIndex];

                    s_stateChanged = false;

                    ++s_scriptDepth;
                    this->handleButton(act.push, act.button, act.player1);
                    --s_scriptDepth;

                    // If checkpoint load/reset happened during this action,
                    // do not continue processing old queued actions in same frame.
                    if (s_stateChanged) {
                        break;
                    }

                    ++s_actionIndex;
                }

                // Auto-disable after macro is fully played.
                // Small delay avoids instantly releasing the last button.
                if (
                    !s_actions.empty() &&
                    s_actionIndex >= s_actions.size() &&
                    s_time >= s_actions.back().time + 0.25
                ) {
                    setBotMode(BotMode::Disabled, false, false);
                }
            }
        }

        PlayLayer::update(dt);
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();

        if (cp && s_mode != BotMode::Disabled && !s_blockRealInput) {
            CheckpointSnapshot snap;

            snap.time = s_time;

            // In Record mode: how many actions were recorded so far.
            // In Play mode: how many actions have already been replayed.
            snap.actionCount =
                (s_mode == BotMode::Play)
                    ? s_actionIndex
                    : s_actions.size();

            snap.pressed = getCurrentPressedKeys();

            // Avoid duplicate stack entries if pointer somehow already exists.
            if (s_snapshots.find(cp) == s_snapshots.end()) {
                s_checkpointStack.push_back(cp);
            }

            s_snapshots[cp] = snap;
        }

        return cp;
    }

    void removeCheckpoint(bool p0) {
        PlayLayer::removeCheckpoint(p0);

        // Do not modify checkpoint stack while loadFromCheckpoint is running.
        if (s_loadDepth > 0) {
            return;
        }

        // This assumes removeCheckpoint removes the latest checkpoint.
        // If your GD version removes arbitrary checkpoints, this needs rework.
        if (!s_checkpointStack.empty()) {
            auto cp = s_checkpointStack.back();
            s_checkpointStack.pop_back();
            s_snapshots.erase(cp);
        }
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        ++s_loadDepth;
        PlayLayer::loadFromCheckpoint(cp);
        --s_loadDepth;

        truncateCheckpointsAfter(cp);

        if (!cp) {
            this->releaseAllButtons();

            s_time = 0.0;
            s_actionIndex = 0;

            clearCheckpointState();
            s_stateChanged = true;
            return;
        }

        auto it = s_snapshots.find(cp);

        // Unknown checkpoint: do not leave Play mode stuck.
        if (it == s_snapshots.end()) {
            this->releaseAllButtons();
            setBotMode(BotMode::Disabled, false, false);
            s_stateChanged = true;
            return;
        }

        auto snap = it->second;

        s_time = snap.time;

        if (s_mode == BotMode::Record) {
            // Remove actions recorded after this checkpoint.
            if (snap.actionCount <= s_actions.size()) {
                s_actions.resize(snap.actionCount);
            }

            s_actionIndex = 0;
        } else if (s_mode == BotMode::Play) {
            // Resume replay from the checkpoint action index.
            s_actionIndex = std::min(snap.actionCount, s_actions.size());
        } else {
            s_actionIndex = 0;
        }

        // Restore physical/logical button state.
        this->releaseAllButtons();
        this->applyButtons(snap.pressed);

        s_stateChanged = true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        // If resetLevel is called from inside loadFromCheckpoint,
        // do not destroy checkpoint state.
        if (s_loadDepth > 0) {
            return;
        }

        s_time = 0.0;
        s_actionIndex = 0;

        clearCheckpointState();
        s_pressed.clear();

        // New recording starts clean.
        // Playback and Disabled keep stored macro.
        if (s_mode == BotMode::Record) {
            s_actions.clear();
        }

        s_stateChanged = true;
    }

    void onExit() {
        PlayLayer::onExit();

        // Leaving level: clear everything, because macro is level-specific.
        s_mode = BotMode::Disabled;

        s_actions.clear();
        s_time = 0.0;
        s_actionIndex = 0;

        clearCheckpointState();
        s_pressed.clear();

        s_pendingRestart = false;
        s_pendingRelease = false;
        s_blockRealInput = false;
        s_stateChanged = false;

        refreshBotUI();
    }
};

// --------------------------------------------------
// 3. PauseLayer UI: separate buttons
// --------------------------------------------------

class $modify(BotPauseLayer, PauseLayer) {
public:
    void customSetup();
    void onExit();

    void onOffButton(CCObject* sender);
    void onRecButton(CCObject* sender);
    void onPlayButton(CCObject* sender);
    void onClearButton(CCObject* sender);

    void customSetup() {
        PauseLayer::customSetup();

        s_pauseNode = this;

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        auto menu = CCMenu::create();
        menu->setPosition({ winSize.width / 2.f, 35.f });
        menu->setID("bot-control-menu");

        auto addButton = [&](
            char const* text,
            char const* idName,
            SEL_MenuHandler selector
        ) {
            auto spr = ButtonSprite::create(text);
            if (!spr) {
                return;
            }

            spr->setScale(0.7f);

            auto item = CCMenuItemSpriteExtra::create(
                spr,
                this,
                selector
            );

            if (!item) {
                return;
            }

            item->setID(idName);
            menu->addChild(item);
        };

        addButton("OFF", "bot-off-btn", menu_selector(BotPauseLayer::onOffButton));
        addButton("REC", "bot-rec-btn", menu_selector(BotPauseLayer::onRecButton));
        addButton("PLAY", "bot-play-btn", menu_selector(BotPauseLayer::onPlayButton));
        addButton("CLEAR", "bot-clear-btn", menu_selector(BotPauseLayer::onClearButton));

        auto layout = RowLayout::create();
        layout->setGap(10.f);

        menu->setLayout(layout);
        menu->updateLayout();

        this->addChild(menu);

        refreshBotUI();
    }

    void onExit() {
        PauseLayer::onExit();

        if (s_pauseNode == this) {
            s_pauseNode = nullptr;
        }
    }

    void onOffButton(CCObject*) {
        setBotMode(BotMode::Disabled, false, false);
        Notification::create("Bot: OFF", NotificationIcon::Warning)->show();
    }

    void onRecButton(CCObject*) {
        setBotMode(BotMode::Record, true, true);
        Notification::create(
            "Bot: RECORDING. Level will restart.",
            NotificationIcon::Info
        )->show();
    }

    void onPlayButton(CCObject*) {
        if (s_actions.empty()) {
            Notification::create(
                "Bot: no macro to play!",
                NotificationIcon::Warning
            )->show();
            return;
        }

        setBotMode(BotMode::Play, true, false);
        Notification::create(
            "Bot: PLAYING. Level will restart.",
            NotificationIcon::Success
        )->show();
    }

    void onClearButton(CCObject*) {
        s_actions.clear();
        setBotMode(BotMode::Disabled, false, false);

        Notification::create(
            "Macro cleared!",
            NotificationIcon::Success
        )->show();
    }
};
