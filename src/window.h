#pragma once
#include "engine.h"
#include "games.h"
#include "webview.h"
#include <QtWidgets>
#include <QtXml/QDomDocument>
#include <memory>

namespace Ui {
class MainWindow;
}

class LibraryFilter;
class SteamSetupDialog;

struct ServerCollectionEntry {
    QString id;
    QString title;
    QStringList children;
};

struct ServerModRecord {
    QString id;
    QString title;
    QString sourceCollectionId;
    qint64 updated = 0;
    qint64 remoteUpdated = 0;
    qint64 installedAt = 0;
    QStringList installedPaths;
};

struct ServerProfile {
    QString id;
    QString name;
    QString appId;
    QString modsFolder;
    QString installFormat = QStringLiteral("subfolders");
    bool cleanRemoved = false;
    QList<ServerCollectionEntry> collections;
    QStringList directModIds;
    QHash<QString, ServerModRecord> mods;
};

class Window : public QMainWindow {
    Q_OBJECT
  public:
    Window(ItemModel &queueModel, ItemModel &libraryModel, Engine &downloadEngine);
    ~Window() override;

  protected:
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    std::unique_ptr<Ui::MainWindow> ui;
    ItemModel &queue, &library;
    Engine &engine;
    QTabWidget *tabs = nullptr;
    QTableView *queueView = nullptr, *libraryView = nullptr, *browserQueue = nullptr;
    QWidget *browserItemPanel = nullptr;
    LibraryFilter *libraryFilter = nullptr;
    QLineEdit *input = nullptr, *search = nullptr, *address = nullptr, *downloadFolderEdit = nullptr;
    QLabel *summary = nullptr, *activity = nullptr, *details = nullptr;
    QPointer<SteamSetupDialog> steamSetupDialog;
    QLabel *pageTitle = nullptr, *pageStatus = nullptr, *browserCount = nullptr;
    QProgressBar *progress = nullptr;
    QPlainTextEdit *log = nullptr;
    QPushButton *startButton = nullptr, *stopButton = nullptr, *cancelButton = nullptr;
    QPushButton *changeDownloadFolderButton = nullptr, *openDownloadFolderButton = nullptr;
    QPushButton *browserAdd = nullptr, *browserDownload = nullptr, *browserStart = nullptr,
                *browserCancel = nullptr;
    QPushButton *browserSelected = nullptr, *browserRetry = nullptr, *browserStop = nullptr,
                *browserRemove = nullptr;
    QWidget *browserAdvanced = nullptr;
    QToolButton *queueToolsToggle = nullptr;
    QAction *accountAction = nullptr;
    QString activeLayoutKey;
    QHash<QString, QByteArray> sessionLayouts;
    QString layoutSettingsKey(int index) const;
    void createMenus();
    void saveTabLayout();
    void restoreTabLayout(int index);
    QString browserPreviewId, browserPreviewTitle;
    bool browserIsCollection = false;
    quint64 browserPreviewGeneration = 0;
    QTimer browserMetadataTimer;
    QToolButton *backButton = nullptr, *forwardButton = nullptr, *reloadButton = nullptr;
    BrowserView *web = nullptr;
    QSplitter *browserSplitter = nullptr;
    QTimer browserLoadTimer;
    QStringList downloadWhenReady;
    QString lastCompletedPath;
    QStringList automaticInstalls;
    bool autoStartScheduled = false;
    bool dependenciesReviewed = false;
    QHash<QString, QSet<QString>> collectionHistory;
    bool diskBusy = false;
    QHash<QString, QString> discoveredGameFolders;
    QTableWidget *installedView = nullptr;
    QLabel *installedNote = nullptr;
    QComboBox *libraryGameFilter = nullptr;
    QListWidget *modGames = nullptr;
    QLabel *modFolder = nullptr;
    QPushButton *modActivation = nullptr, *modLauncher = nullptr, *setModFolderButton = nullptr;
    QListWidget *serverList = nullptr;
    QPushButton *addServerButton = nullptr, *editServerButton = nullptr, *removeServerButton = nullptr;
    QPushButton *addServerModsButton = nullptr, *syncServerButton = nullptr, *syncAllServersButton = nullptr,
                *checkServerUpdatesButton = nullptr, *setServerFolderButton = nullptr,
                *openServerFolderButton = nullptr, *removeServerModsButton = nullptr;
    QLabel *serverFolderLabel = nullptr, *serverNoteLabel = nullptr;
    QTableWidget *serverModsView = nullptr;
    QList<ServerProfile> serverProfiles;
    QString activeServerSyncId;
    QStringList pendingServerSyncIds;
    QStringList activeServerInstallModIds;
    QString selectedModGame() const;
    void gameActivation();
    void setModGameFolder();
    void changeDownloadFolder();
    void setupDownloads();
    void setupBrowser();
    void setupLibrary();
    void setupDownloadMethods();
    void setupServers();
    void loadServerProfiles();
    bool saveServerProfiles();
    QString selectedServerId() const;
    ServerProfile *findServerProfile(const QString &id);
    const ServerProfile *findServerProfile(const QString &id) const;
    void reloadServers();
    void editServerDialog(const QString &serverId = {});
    void deleteServerProfile();
    void addServerModsDialog();
    void addEntriesToServer(const QString &serverId, const QStringList &rawIds, bool syncAfter = false);
    void setServerModsFolder();
    void openServerModsFolder();
    void removeSelectedServerMods();
    void checkServerUpdates(const QString &serverId = {}, bool syncAfter = false);
    void syncServerProfile(const QString &serverId);
    void syncAllServerProfiles();
    void installServerModsNow(const QString &serverId, const QStringList &modIds);
    void finishServerDownloadRun();
    void serverContextMenu(const QPoint &pos);
    void configureTable(QTableView *view, ItemModel *model);
    QStringList selected(QTableView *view, ItemModel *model) const;
    void addText(const QString &text);
    void refresh();
    void settingsDialog();
    void accountDialog();
    void guardDialog(const QString &prompt);
    void collectionDialog(const QString &id, const QString &title, const QStringList &children);
    void queueMenu(const QPoint &pos, QTableView *view);
    void libraryMenu(const QPoint &pos);
    void installSelected();
    void openSelected(bool website);
    void deleteSelected();
    void appendLog(const QString &text);
    void navigate(const QUrl &url);
    void syncBrowser();
    void downloadPage(bool now);
    void maybeStartDownloads();
    void importList();
    void exportList();
    void collectionImport();
    void removeQueue(QTableView *view);
    QString queueFolder(const QString &id) const;
    void openFolder(const QString &path);
    void startQueue(QTableView *selection = nullptr);
    void retryFailed();
    void clearCompleted();
    void moveQueueSelection(QTableView *view, int direction);
    void showLog();
    void showSupport();
    void gameTools();
    void setupInstalled();
    void reloadInstalled();
    void installedAction(const QString &action);
    void installItems(const QStringList &ids, bool automatic = false);
    void checkUpdates(bool quiet = false);
    void pasteMultiple();
    void browseGame();
    void fillBookmarks(QMenu *menu);
    void editBookmark(const QString &id = {});
    void activationDialog();
    void importLocalMod(bool folder);
    void recoverLibrary();
    void cleanCache();
    void showHistory();
    void addDependencies(const QStringList &ids, bool download = false);
};
