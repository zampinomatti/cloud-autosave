#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <array>
#include <chrono>
 
using namespace geode::prelude;
 
// Account values shown on the profile / stats screen:
// stars, moons, demons, secret coins, user coins, diamonds.
using Stats = std::array<int, 6>;
 
static Stats readStats() {
    auto gsm = GameStatsManager::sharedState();
    return {
        gsm->getStat("6"),  // stars
        gsm->getStat("28"), // moons
        gsm->getStat("5"),  // demons
        gsm->getStat("8"),  // secret coins
        gsm->getStat("12"), // user coins
        gsm->getStat("13"), // diamonds
    };
}
 
// Does the same thing as the "Save" button in the account menu, but automatically:
// AppDelegate::trySaveGame -> GJAccountManager::getAccountBackupURL (the game gets the
// upload URL and uploads the save) -> result comes back through this delegate.
class CloudAutosave final : public GJAccountBackupDelegate {
    using Clock = std::chrono::steady_clock;
 
    Clock::time_point m_nextAllowed = Clock::time_point{}; // no wait for the first save
    Clock::time_point m_busySince = Clock::time_point{};
    bool m_busy = false;
    bool m_hasBaseline = false;
    bool m_newBest = false;
    bool m_inflightNewBest = false;
    int m_failures = 0;
    Stats m_baseline{};  // stats as of the last successful save
    Stats m_inflight{};  // stats at the moment the current save started
 
    static void notify(char const* text, NotificationIcon icon) {
        if (Mod::get()->getSettingValue<bool>("show-notifications")) {
            Notification::create(text, icon)->show();
        }
    }
 
    static int64_t cooldownSeconds() {
        return Mod::get()->getSettingValue<int64_t>("cooldown");
    }
 
    void finish() {
        m_busy = false;
        auto acc = GJAccountManager::get();
        if (acc->m_backupDelegate == this) {
            acc->m_backupDelegate = nullptr;
        }
    }
 
public:
    static CloudAutosave* get() {
        static CloudAutosave instance;
        return &instance;
    }
 
    // Called once when the main menu opens (stats are loaded by then).
    void init() {
        if (!m_hasBaseline) {
            m_baseline = readStats();
            m_hasBaseline = true;
        }
    }
 
    void flagNewBest() {
        m_newBest = true;
    }
 
    void tick() {
        if (!m_hasBaseline) return;
 
        auto now = Clock::now();
 
        // Safety net: if the game never answered, do not stay blocked forever.
        if (m_busy) {
            if (now - m_busySince > std::chrono::seconds(60)) {
                log::warn("Cloud save got no answer, giving up on it");
                this->backupAccountFailed(BackupAccountError::GenericError, 0);
            }
            return;
        }
 
        auto mod = Mod::get();
        bool statsChanged = mod->getSettingValue<bool>("save-on-stat-change") && readStats() != m_baseline;
        bool newBest = m_newBest && mod->getSettingValue<bool>("save-on-new-best");
        if (!statsChanged && !newBest) return;
 
        auto acc = GJAccountManager::get();
        if (acc->m_accountID <= 0) return; // not logged in
 
        // If the account menu is open it handles saving/loading itself: do not interfere.
        auto scene = CCDirector::get()->getRunningScene();
        if (scene && scene->getChildByType<AccountLayer>(0)) return;
 
        if (now < m_nextAllowed) return;
        m_nextAllowed = now + std::chrono::seconds(cooldownSeconds());
 
        m_busy = true;
        m_busySince = now;
        m_inflight = readStats();
        m_inflightNewBest = m_newBest;
        m_newBest = false;
 
        log::info("Starting cloud save ({})", newBest ? "new best" : "stats changed");
        AppDelegate::get()->trySaveGame(true); // write the save data to disk before uploading
        acc->m_backupDelegate = this;
        if (!acc->getAccountBackupURL()) {
            this->backupAccountFailed(BackupAccountError::GenericError, 0);
        }
    }
 
    void backupAccountFinished() override {
        this->finish();
        m_baseline = m_inflight;
        m_failures = 0;
        log::info("Cloud save completed");
        notify("Account saved to the cloud", NotificationIcon::Success);
    }
 
    void backupAccountFailed(BackupAccountError type, int response) override {
        this->finish();
        m_newBest = m_newBest || m_inflightNewBest; // retry later
        m_failures++;
        // Wait longer after each failure (up to 15 minutes) instead of hammering the servers.
        int64_t wait = std::min<int64_t>(cooldownSeconds() << std::min(m_failures, 5), 900);
        m_nextAllowed = Clock::now() + std::chrono::seconds(wait);
        log::warn("Cloud save failed (type {}, response {}), retrying in {}s", static_cast<int>(type), response, wait);
        notify("Cloud save failed", NotificationIcon::Error);
    }
};
 
// Checks every few seconds whether something worth saving has happened.
class AutosaveTicker : public CCObject {
public:
    void tick(float) {
        CloudAutosave::get()->tick();
    }
};
 
class $modify(AutosaveMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
 
        // The director only exists now, so the timer starts the first time the menu opens.
        static bool started = false;
        if (!started) {
            started = true;
            CloudAutosave::get()->init();
            auto ticker = new AutosaveTicker(); // lives for the whole session, on purpose
            CCDirector::get()->getScheduler()->scheduleSelector(
                schedule_selector(AutosaveTicker::tick), ticker, 3.f, false
            );
        }
        return true;
    }
};
 
class $modify(AutosavePlayLayer, PlayLayer) {
    // Called by the game when you get a new best.
    void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
        if (Mod::get()->getSettingValue<bool>("save-on-new-best")) {
            CloudAutosave::get()->flagNewBest();
        }
    }
};
 
        }
    }
};
