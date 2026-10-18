#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <chrono>

using namespace geode::prelude;

// Fa lo stesso backup del pulsante "Save" nel menu account, ma in automatico.
// Usa la stessa catena del gioco: AppDelegate::trySaveGame -> GJAccountManager::getAccountBackupURL
// (che ottiene l'URL e poi chiama backupAccount) -> callback sul delegate.
class CloudAutosave final : public GJAccountBackupDelegate {
    using Clock = std::chrono::steady_clock;

    Clock::time_point m_lastAttempt = Clock::now();
    bool m_busy = false;
    // Si fa il backup solo se il giocatore ha effettivamente giocato:
    // così non si sovrascrive il cloud con dati locali "freschi" appena avviato il gioco.
    bool m_dirty = false;

    static void notify(char const* text, NotificationIcon icon) {
        if (Mod::get()->getSettingValue<bool>("show-notifications")) {
            Notification::create(text, icon)->show();
        }
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

    void markDirty() {
        m_dirty = true;
    }

    void tryBackup(bool fromLevelExit = false) {
        if (m_busy || !m_dirty) return;

        // Niente backup durante una partita o nell'editor (evita lag), tranne all'uscita da un livello.
        if (!fromLevelExit && (PlayLayer::get() || LevelEditorLayer::get())) return;

        auto acc = GJAccountManager::get();
        if (acc->m_accountID <= 0) return; // non hai fatto il login

        // Se il menu account e' aperto, ci pensa lui: non rubiamo il delegate.
        auto scene = CCDirector::get()->getRunningScene();
        if (scene && scene->getChildByType<AccountLayer>(0)) return;

        auto interval = std::chrono::minutes(Mod::get()->getSettingValue<int64_t>("interval-minutes"));
        auto now = Clock::now();
        if (now - m_lastAttempt < interval) return;
        m_lastAttempt = now;

        m_busy = true;
        AppDelegate::get()->trySaveGame(true); // scrive i dati su disco prima dell'upload
        acc->m_backupDelegate = this;
        if (!acc->getAccountBackupURL()) {
            this->backupAccountFailed(BackupAccountError::GenericError, 0);
        }
    }

    void backupAccountFinished() override {
        this->finish();
        m_dirty = false;
        log::info("Backup cloud completato");
        notify("Account salvato sul cloud", NotificationIcon::Success);
    }

    void backupAccountFailed(BackupAccountError type, int response) override {
        this->finish();
        log::warn("Backup cloud fallito (tipo {}, risposta {})", static_cast<int>(type), response);
        notify("Backup cloud fallito", NotificationIcon::Error);
    }
};

// Controlla ogni minuto se e' il momento di fare il backup.
class AutosaveTicker : public CCObject {
public:
    void tick(float) {
        CloudAutosave::get()->tryBackup();
    }
};

class $modify(AutosaveMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        // Il director esiste solo ora, quindi il timer parte alla prima apertura del menu.
        static bool started = false;
        if (!started) {
            started = true;
            auto ticker = new AutosaveTicker(); // vive per tutta la sessione, di proposito
            CCDirector::get()->getScheduler()->scheduleSelector(
                schedule_selector(AutosaveTicker::tick), ticker, 60.f, false
            );
        }
        return true;
    }
};

class $modify(AutosavePlayLayer, PlayLayer) {
    void levelComplete() {
        PlayLayer::levelComplete();
        CloudAutosave::get()->markDirty();
    }

    void onQuit() {
        PlayLayer::onQuit();
        CloudAutosave::get()->markDirty();
        if (Mod::get()->getSettingValue<bool>("on-level-exit")) {
            CloudAutosave::get()->tryBackup(true);
        }
    }
};
