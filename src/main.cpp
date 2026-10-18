#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <array>
#include <chrono>
 
using namespace geode::prelude;

using Stats = std::array<int, 6>;
 
static Stats readStats() {
    auto gsm = GameStatsManager::sharedState();
    return {
        gsm->getStat("6"),
        gsm->getStat("28"),
        gsm->getStat("5"),
        gsm->getStat("8"),
        gsm->getStat("12"),
        gsm->getStat("13"),
    };
}
 
static bool isSafeMoment() {
    if (!PlayLayer::get()) return true;
    auto scene = CCDirector::get()->getRunningScene();
    return scene && scene->getChildByType<PauseLayer>(0) != nullptr;
}
 
class CloudAutosave final : public GJAccountBackupDelegate {
    using Clock = std::chrono::steady_clock;
 
    static constexpr auto SETTLE_TIME = std::chrono::seconds(2);
 
    Clock::time_point m_nextAllowed = Clock::time_point{};
    Clock::time_point m_busySince = Clock::time_point{};
    Clock::time_point m_lastChange = Clock::time_point{};
    bool m_busy = false;
    bool m_hasBaseline = false;
    bool m_newBest = false;
    bool m_inflightNewBest = false;
    int m_failures = 0;
    Stats m_baseline{};
    Stats m_inflight{};
    Stats m_lastSeen{}; 
 
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
 
    void init() {
        if (m_hasBaseline) return;
        m_baseline = readStats();
        m_lastSeen = m_baseline;
        m_lastChange = Clock::now();
        m_hasBaseline = true;
    }
 
    void flagNewBest() {
        m_newBest = true;
    }
 
    void tick() {
        if (!m_hasBaseline) return;
 
        auto now = Clock::now();
 
        auto cur = readStats();
        if (cur != m_lastSeen) {
            m_lastSeen = cur;
            m_lastChange = now;
        }
 
        if (m_busy) {
            if (now - m_busySince > std::chrono::seconds(60)) {
                log::warn("Cloud save got no answer, giving up on it");
                this->backupAccountFailed(BackupAccountError::GenericError, 0);
            }
            return;
        }
 
        auto mod = Mod::get();
        bool statsChanged = mod->getSettingValue<bool>("save-on-stat-change") && cur != m_baseline;
        bool newBest = m_newBest && mod->getSettingValue<bool>("save-on-new-best");
        if (!statsChanged && !newBest) return;
 
        if (!isSafeMoment()) return;
 
        if (now - m_lastChange < SETTLE_TIME) return;
 
        if (now < m_nextAllowed) return;
 
        auto acc = GJAccountManager::get();
        if (acc->m_accountID <= 0) return;
 
        auto scene = CCDirector::get()->getRunningScene();
        if (scene && scene->getChildByType<AccountLayer>(0)) return;
 
        m_nextAllowed = now + std::chrono::seconds(cooldownSeconds());
 
        m_busy = true;
        m_busySince = now;
        m_inflight = cur;
        m_inflightNewBest = m_newBest;
        m_newBest = false;
 
        log::info("Starting cloud save (new best: {}, stats changed: {})", newBest, statsChanged);
        AppDelegate::get()->trySaveGame(true);
        acc->m_backupDelegate = this;
        if (!acc->getAccountBackupURL()) {
            this->backupAccountFailed(BackupAccountError::GenericError, 0);
        }
    }
 
    void backupAccountFinished() override {
        this->finish();
        m_baseline = m_inflight;
        m_failures = 0;
        Clock::time_point next = Clock::now() + std::chrono::seconds(cooldownSeconds());
        m_nextAllowed = std::max(m_nextAllowed, next);
        log::info("Cloud save completed");
        notify("Account saved to the cloud", NotificationIcon::Success);
    }
 
    void backupAccountFailed(BackupAccountError type, int response) override {
        this->finish();
        m_newBest = m_newBest || m_inflightNewBest;
        m_failures++;
        int64_t wait = std::min<int64_t>(cooldownSeconds() << std::min(m_failures, 5), 900);
        m_nextAllowed = Clock::now() + std::chrono::seconds(wait);
        log::warn("Cloud save failed (type {}, response {}), retrying in {}s", static_cast<int>(type), response, wait);
        notify("Cloud save failed", NotificationIcon::Error);
    }
};
 
class AutosaveTicker : public CCObject {
public:
    void tick(float) {
        CloudAutosave::get()->tick();
    }
};
 
class $modify(AutosaveMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
 
        static bool started = false;
        if (!started) {
            started = true;
            CloudAutosave::get()->init();
            auto ticker = new AutosaveTicker();
            CCDirector::get()->getScheduler()->scheduleSelector(
                schedule_selector(AutosaveTicker::tick), ticker, 1.f, false
            );
        }
        return true;
    }
};
 
class $modify(AutosavePlayLayer, PlayLayer) {
    void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
        if (Mod::get()->getSettingValue<bool>("save-on-new-best")) {
            CloudAutosave::get()->flagNewBest();
        }
    }
};
 
