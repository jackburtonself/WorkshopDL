#include "window.h"
#include "gma.h"
#include "ui_mainwindow.h"

namespace {
QString serversFilePath() {
    return dataRoot() + QStringLiteral("/servers.json");
}

QString normalizeWorkshopAppId(const QString &appId) {
    if (appId == QStringLiteral("1026340"))
        return QStringLiteral("602960"); // Barotrauma Dedicated Server -> Barotrauma Workshop
    return appId;
}

QString sanitizeFolderTitle(QString title) {
    title.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]")), QStringLiteral("_"));
    title = title.simplified().replace(QLatin1Char(' '), QLatin1Char('_'));
    while (title.endsWith(QLatin1Char('.')) || title.endsWith(QLatin1Char('_')))
        title.chop(1);
    if (title.size() > 64)
        title = title.left(64);
    return title;
}

QString layoutLabel(const QString &format) {
    if (format == QStringLiteral("game_profile"))
        return QStringLiteral("Game package format (.tmod, .gma, .vpk, .pak)");
    if (format == QStringLiteral("id_title"))
        return QStringLiteral("Titled subfolders (<ModsFolder>/<ID>_<Title>)");
    return QStringLiteral("Mod ID subfolders (<ModsFolder>/<ID>)");
}

QString expectedServerModPath(const ServerProfile &server, const ServerModRecord &mod) {
    const QString base = QDir::cleanPath(server.modsFolder);
    if (!mod.installedPaths.isEmpty())
        return base + QLatin1Char('/') + mod.installedPaths.first();
    if (server.installFormat == QStringLiteral("id_title") && !mod.title.isEmpty()) {
        const QString cleanTitle = sanitizeFolderTitle(mod.title);
        if (!cleanTitle.isEmpty())
            return base + QLatin1Char('/') + mod.id + QLatin1Char('_') + cleanTitle;
    }
    return base + QLatin1Char('/') + mod.id;
}

bool serverModFilesExist(const ServerProfile &server, const ServerModRecord &mod) {
    const QString base = QDir::cleanPath(server.modsFolder);
    if (base.isEmpty() || !QDir(base).exists())
        return false;
    if (!mod.installedPaths.isEmpty()) {
        for (const auto &rel : mod.installedPaths)
            if (!QFileInfo::exists(base + QLatin1Char('/') + rel))
                return false;
        return true;
    }
    if (server.installFormat == QStringLiteral("subfolders"))
        return QDir(base + QLatin1Char('/') + mod.id).exists();
    if (server.installFormat == QStringLiteral("id_title")) {
        const QString cleanTitle = sanitizeFolderTitle(mod.title);
        const QString folder = cleanTitle.isEmpty() ? mod.id : (mod.id + QLatin1Char('_') + cleanTitle);
        return QDir(base + QLatin1Char('/') + folder).exists();
    }
    return false;
}

bool removeServerModFiles(const ServerProfile &server, const ServerModRecord &mod) {
    const QString base = QDir::cleanPath(server.modsFolder);
    if (base.isEmpty() || !QDir(base).exists())
        return true;
    QStringList targets = mod.installedPaths;
    if (targets.isEmpty() && server.installFormat == QStringLiteral("subfolders"))
        targets << mod.id;
    bool ok = true;
    for (const auto &rel : targets) {
        if (rel.isEmpty() || rel.contains(QStringLiteral("..")) || QDir::isAbsolutePath(rel))
            continue;
        const QString full = base + QLatin1Char('/') + rel;
        if (!QFileInfo::exists(full))
            continue;
        if (QFileInfo(full).isDir())
            ok = QDir(full).removeRecursively() && ok;
        else
            ok = QFile::remove(full) && ok;
    }
    return ok;
}

struct ServerInstallOutcome {
    QString modId;
    QStringList installedPaths;
    qint64 updated = 0;
    QString error;
};

ServerInstallOutcome installOneServerMod(const ServerProfile &server, const ServerModRecord &mod,
                                         const Item &sourceItem) {
    ServerInstallOutcome result;
    result.modId = mod.id;
    result.updated = qMax(mod.remoteUpdated, sourceItem.updated);
    const QString srcPath = QDir::cleanPath(sourceItem.path);
    const QString destRoot = QDir::cleanPath(server.modsFolder);
    if (srcPath.isEmpty() || !QDir(srcPath).exists()) {
        result.error = QStringLiteral("Downloaded files not found at %1").arg(srcPath);
        return result;
    }
    if (destRoot.isEmpty() || !QDir().mkpath(destRoot)) {
        result.error = QStringLiteral("Cannot create server mods folder: %1").arg(destRoot);
        return result;
    }

    auto copyFolderSafe = [&](const QString &fromDir, const QString &relFolder) -> QString {
        const QString target = destRoot + QLatin1Char('/') + relFolder;
        if (QDir::cleanPath(fromDir).compare(QDir::cleanPath(target), Qt::CaseInsensitive) == 0) {
            result.installedPaths << relFolder;
            return {};
        }
        const QString stage = target + QStringLiteral(".workshopdl-stage");
        const QString backup = target + QStringLiteral(".workshopdl-prev");
        if (QDir(stage).exists() && !QDir(stage).removeRecursively())
            return QStringLiteral("Cannot clear staging folder: ") + stage;
        QString copyErr;
        if (!copyTree(fromDir, stage, &copyErr)) {
            QDir(stage).removeRecursively();
            return copyErr;
        }
        if (QDir(backup).exists() && !QDir(backup).removeRecursively()) {
            QDir(stage).removeRecursively();
            return QStringLiteral("Cannot remove previous backup folder.");
        }
        const bool existed = QDir(target).exists();
        if (existed && !QDir().rename(target, backup)) {
            QDir(stage).removeRecursively();
            return QStringLiteral("Cannot replace %1. Close the server if it is locking the files.")
                .arg(relFolder);
        }
        if (!QDir().rename(stage, target)) {
            if (existed)
                QDir().rename(backup, target);
            QDir(stage).removeRecursively();
            return QStringLiteral("Cannot finalize installation into ") + target;
        }
        if (existed)
            QDir(backup).removeRecursively();
        result.installedPaths << relFolder;
        return {};
    };

    if (server.installFormat == QStringLiteral("game_profile")) {
        const QString effectiveAppId =
            validId(server.appId) ? normalizeWorkshopAppId(server.appId) : sourceItem.appId;
        const auto profile = profileFor(effectiveAppId);
        if (profile.format.startsWith(QStringLiteral("files:"))) {
            QStringList filters;
            for (const auto &ext : profile.format.mid(6).split(QLatin1Char(',')))
                filters << QLatin1Char('*') + ext;
            QDirIterator it(srcPath, filters, QDir::Files, QDirIterator::Subdirectories);
            QHash<QString, QString> bestByFilename;
            QHash<QString, QString> originalCase;
            while (it.hasNext()) {
                const QString file = it.next();
                const QString name = QFileInfo(file).fileName();
                const QString folded = name.toCaseFolded();
                if (!bestByFilename.contains(folded)) {
                    bestByFilename.insert(folded, file);
                    originalCase.insert(folded, name);
                } else {
                    const QString prev = bestByFilename.value(folded);
                    const auto prevVer = QVersionNumber::fromString(QFileInfo(prev).dir().dirName());
                    const auto curVer = QVersionNumber::fromString(QFileInfo(file).dir().dirName());
                    if ((!curVer.isNull() && (prevVer.isNull() || curVer > prevVer)) ||
                        (curVer == prevVer &&
                         QFileInfo(file).lastModified() >= QFileInfo(prev).lastModified())) {
                        bestByFilename.insert(folded, file);
                        originalCase.insert(folded, name);
                    }
                }
            }
            if (bestByFilename.isEmpty()) {
                result.error =
                    QStringLiteral("No %1 package files found in mod %2.").arg(profile.format.mid(6), mod.id);
                return result;
            }
            for (auto mapIt = bestByFilename.cbegin(); mapIt != bestByFilename.cend(); ++mapIt) {
                const QString filename = originalCase.value(mapIt.key());
                const QString destFile = destRoot + QLatin1Char('/') + filename;
                const QString tempFile = destFile + QStringLiteral(".workshopdl-tmp");
                QFile::remove(tempFile);
                if (!QFile::copy(mapIt.value(), tempFile)) {
                    result.error = QStringLiteral("Could not copy %1 to server folder.").arg(filename);
                    return result;
                }
                if (QFileInfo::exists(destFile) && !QFile::remove(destFile)) {
                    QFile::remove(tempFile);
                    result.error =
                        QStringLiteral("Could not overwrite %1 (file may be in use by server).").arg(filename);
                    return result;
                }
                if (!QFile::rename(tempFile, destFile)) {
                    QFile::remove(tempFile);
                    result.error = QStringLiteral("Could not finalize %1.").arg(filename);
                    return result;
                }
                result.installedPaths << filename;
            }
            return result;
        } else if (profile.format == QStringLiteral("gma")) {
            QStringList archives;
            QDirIterator it(srcPath, {QStringLiteral("*.gma"), QStringLiteral("*.bin")}, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
                archives << it.next();
            if (!archives.isEmpty()) {
                for (int n = 0; n < archives.size(); ++n) {
                    const QString folderName =
                        QStringLiteral("WorkshopDL-") + mod.id +
                        (archives.size() > 1 ? QStringLiteral("-%1").arg(n + 1) : QString());
                    const QString target = destRoot + QLatin1Char('/') + folderName;
                    if (QDir(target).exists() && !QDir(target).removeRecursively()) {
                        result.error = QStringLiteral("Could not replace existing GMA folder ") + folderName;
                        return result;
                    }
                    const QString err = extractGma(archives[n], target);
                    if (!err.isEmpty()) {
                        result.error = err;
                        return result;
                    }
                    result.installedPaths << folderName;
                }
                return result;
            }
        }
    }

    QString folderName = mod.id;
    if (server.installFormat == QStringLiteral("id_title") && !mod.title.isEmpty()) {
        const QString cleanTitle = sanitizeFolderTitle(mod.title);
        if (!cleanTitle.isEmpty())
            folderName = mod.id + QLatin1Char('_') + cleanTitle;
    }
    if (!mod.installedPaths.isEmpty() && mod.installedPaths.first() != folderName)
        removeServerModFiles(server, mod);
    result.error = copyFolderSafe(srcPath, folderName);
    return result;
}
} // namespace

void Window::loadServerProfiles() {
    serverProfiles.clear();
    const auto root = readObject(serversFilePath());
    for (const auto &val : root.value(QStringLiteral("servers")).toArray()) {
        const auto obj = val.toObject();
        ServerProfile s;
        s.id = obj.value(QStringLiteral("id")).toString();
        s.name = obj.value(QStringLiteral("name")).toString();
        s.appId = obj.value(QStringLiteral("appId")).toString();
        s.modsFolder = obj.value(QStringLiteral("modsFolder")).toString();
        s.installFormat =
            obj.value(QStringLiteral("installFormat")).toString(QStringLiteral("subfolders"));
        s.cleanRemoved = obj.value(QStringLiteral("cleanRemoved")).toBool(false);
        if (s.id.isEmpty() || s.name.isEmpty())
            continue;
        for (const auto &cVal : obj.value(QStringLiteral("collections")).toArray()) {
            const auto cObj = cVal.toObject();
            ServerCollectionEntry col;
            col.id = cObj.value(QStringLiteral("id")).toString();
            col.title = cObj.value(QStringLiteral("title")).toString();
            for (const auto &ch : cObj.value(QStringLiteral("children")).toArray())
                if (validId(ch.toString()))
                    col.children << ch.toString();
            if (validId(col.id))
                s.collections.append(col);
        }
        for (const auto &dVal : obj.value(QStringLiteral("directModIds")).toArray()) {
            const QString id = dVal.toString();
            if (validId(id) && !s.directModIds.contains(id))
                s.directModIds << id;
        }
        const auto modsObj = obj.value(QStringLiteral("mods")).toObject();
        for (auto it = modsObj.begin(); it != modsObj.end(); ++it) {
            const auto mObj = it.value().toObject();
            ServerModRecord m;
            m.id = mObj.value(QStringLiteral("id")).toString(it.key());
            if (!validId(m.id))
                continue;
            m.title = mObj.value(QStringLiteral("title")).toString();
            m.sourceCollectionId = mObj.value(QStringLiteral("sourceCollectionId")).toString();
            m.updated = mObj.value(QStringLiteral("updated")).toString().toLongLong();
            m.remoteUpdated = mObj.value(QStringLiteral("remoteUpdated")).toString().toLongLong();
            m.installedAt = mObj.value(QStringLiteral("installedAt")).toString().toLongLong();
            for (const auto &pVal : mObj.value(QStringLiteral("installedPaths")).toArray())
                if (!pVal.toString().isEmpty())
                    m.installedPaths << pVal.toString();
            s.mods.insert(m.id, m);
        }
        for (const auto &col : s.collections)
            for (const auto &childId : col.children)
                if (!s.mods.contains(childId)) {
                    ServerModRecord m;
                    m.id = childId;
                    m.sourceCollectionId = col.id;
                    s.mods.insert(childId, m);
                }
        for (const auto &modId : s.directModIds)
            if (!s.mods.contains(modId)) {
                ServerModRecord m;
                m.id = modId;
                s.mods.insert(modId, m);
            }
        serverProfiles.append(s);
    }
}

bool Window::saveServerProfiles() {
    QJsonArray arr;
    for (const auto &s : serverProfiles) {
        QJsonArray cols;
        for (const auto &col : s.collections) {
            QJsonArray children;
            for (const auto &ch : col.children)
                children.append(ch);
            cols.append(QJsonObject{{QStringLiteral("id"), col.id},
                                    {QStringLiteral("title"), col.title},
                                    {QStringLiteral("children"), children}});
        }
        QJsonArray direct;
        for (const auto &id : s.directModIds)
            direct.append(id);
        QJsonObject modsObj;
        for (auto it = s.mods.cbegin(); it != s.mods.cend(); ++it) {
            const auto &m = it.value();
            QJsonArray paths;
            for (const auto &p : m.installedPaths)
                paths.append(p);
            modsObj.insert(
                m.id, QJsonObject{{QStringLiteral("id"), m.id},
                                  {QStringLiteral("title"), m.title},
                                  {QStringLiteral("sourceCollectionId"), m.sourceCollectionId},
                                  {QStringLiteral("updated"), QString::number(m.updated)},
                                  {QStringLiteral("remoteUpdated"), QString::number(m.remoteUpdated)},
                                  {QStringLiteral("installedAt"), QString::number(m.installedAt)},
                                  {QStringLiteral("installedPaths"), paths}});
        }
        arr.append(QJsonObject{{QStringLiteral("id"), s.id},
                               {QStringLiteral("name"), s.name},
                               {QStringLiteral("appId"), s.appId},
                               {QStringLiteral("modsFolder"), s.modsFolder},
                               {QStringLiteral("installFormat"), s.installFormat},
                               {QStringLiteral("cleanRemoved"), s.cleanRemoved},
                               {QStringLiteral("collections"), cols},
                               {QStringLiteral("directModIds"), direct},
                               {QStringLiteral("mods"), modsObj}});
    }
    return writeJson(
        serversFilePath(),
        QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("servers"), arr}}));
}

QString Window::selectedServerId() const {
    return serverList && serverList->currentItem()
               ? serverList->currentItem()->data(Qt::UserRole).toString()
               : QString();
}

ServerProfile *Window::findServerProfile(const QString &id) {
    for (auto &s : serverProfiles)
        if (s.id == id)
            return &s;
    return nullptr;
}

const ServerProfile *Window::findServerProfile(const QString &id) const {
    for (const auto &s : serverProfiles)
        if (s.id == id)
            return &s;
    return nullptr;
}

void Window::setupServers() {
    if (!serverList || !serverModsView)
        return;
    loadServerProfiles();
    serverModsView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    serverModsView->setColumnWidth(1, 185);
    serverModsView->setColumnWidth(2, 135);
    serverModsView->setColumnWidth(3, 110);
    serverModsView->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);

    connect(addServerButton, &QPushButton::clicked, this, [this] { editServerDialog({}); });
    connect(editServerButton, &QPushButton::clicked, this, [this] {
        const QString id = selectedServerId();
        if (!id.isEmpty())
            editServerDialog(id);
    });
    connect(removeServerButton, &QPushButton::clicked, this, &Window::deleteServerProfile);
    connect(addServerModsButton, &QPushButton::clicked, this, &Window::addServerModsDialog);
    connect(syncServerButton, &QPushButton::clicked, this, [this] {
        const QString id = selectedServerId();
        if (!id.isEmpty()) {
            pendingServerSyncIds.clear();
            syncServerProfile(id);
        }
    });
    connect(syncAllServersButton, &QPushButton::clicked, this, &Window::syncAllServerProfiles);
    connect(checkServerUpdatesButton, &QPushButton::clicked, this, [this] {
        const QString id = selectedServerId();
        if (!id.isEmpty())
            checkServerUpdates(id, false);
    });
    connect(setServerFolderButton, &QPushButton::clicked, this, &Window::setServerModsFolder);
    connect(openServerFolderButton, &QPushButton::clicked, this, &Window::openServerModsFolder);
    connect(removeServerModsButton, &QPushButton::clicked, this, &Window::removeSelectedServerMods);

    connect(serverList, &QListWidget::currentRowChanged, this, [this] {
        engine.settings.setValue(QStringLiteral("selectedServerProfile"), selectedServerId());
        reloadServers();
        refresh();
    });
    connect(serverList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item)
            editServerDialog(item->data(Qt::UserRole).toString());
    });
    connect(serverModsView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refresh(); });
    connect(serverModsView, &QTableWidget::doubleClicked, this, [this](const QModelIndex &index) {
        const auto *server = findServerProfile(selectedServerId());
        if (!server || index.row() < 0)
            return;
        const QString modId = serverModsView->item(index.row(), 0)->data(Qt::UserRole).toString();
        const auto mod = server->mods.value(modId);
        const QString path = expectedServerModPath(*server, mod);
        if (QFileInfo::exists(path))
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
        else
            navigate(QUrl(workshopUrl(modId)));
    });
    connect(serverModsView, &QTableWidget::customContextMenuRequested, this, &Window::serverContextMenu);
    reloadServers();
}

void Window::reloadServers() {
    if (!serverList || !serverModsView)
        return;
    const QString previous = selectedServerId().isEmpty()
                                 ? engine.settings.value(QStringLiteral("selectedServerProfile")).toString()
                                 : selectedServerId();
    {
        QSignalBlocker block(serverList);
        serverList->clear();
        int selectRow = 0;
        for (int n = 0; n < serverProfiles.size(); ++n) {
            const auto &s = serverProfiles[n];
            auto *item = new QListWidgetItem(
                QStringLiteral("%1 (%2)").arg(s.name).arg(s.mods.size()), serverList);
            item->setData(Qt::UserRole, s.id);
            item->setToolTip(QStringLiteral("App ID: %1 (%2)\nMods folder: %3")
                                 .arg(s.appId, gameName(s.appId), QDir::toNativeSeparators(s.modsFolder)));
            if (s.id == previous)
                selectRow = n;
        }
        if (serverList->count() > 0)
            serverList->setCurrentRow(selectRow);
    }

    serverModsView->setRowCount(0);
    const auto *server = findServerProfile(selectedServerId());
    if (!server) {
        serverFolderLabel->setText(
            QStringLiteral("No server profile selected. Click 'New server…' to create a profile for your "
                           "dedicated server."));
        serverNoteLabel->setText(QStringLiteral(
            "Server Manager lets you maintain separate mod and Workshop collection setups for each server on "
            "your system and keep them updated automatically."));
        return;
    }

    QHash<QString, QString> collectionNames;
    for (const auto &col : server->collections)
        collectionNames.insert(col.id, col.title.isEmpty() ? (QStringLiteral("Collection ") + col.id)
                                                           : col.title);

    serverFolderLabel->setText(
        QStringLiteral("Server: %1   |   Steam App ID: %2 (%3)   |   Mods folder: %4   |   Layout: %5")
            .arg(server->name, server->appId, gameName(server->appId),
                 QDir::toNativeSeparators(server->modsFolder), layoutLabel(server->installFormat)));

    QList<ServerModRecord> ordered = server->mods.values();
    std::sort(ordered.begin(), ordered.end(), [](const ServerModRecord &a, const ServerModRecord &b) {
        const QString titleA = a.title.isEmpty() ? a.id : a.title;
        const QString titleB = b.title.isEmpty() ? b.id : b.title;
        const int cmp = QString::localeAwareCompare(titleA, titleB);
        return cmp == 0 ? a.id < b.id : cmp < 0;
    });

    int installedCount = 0, updateCount = 0, pendingCount = 0;
    for (const auto &mod : ordered) {
        const int row = serverModsView->rowCount();
        serverModsView->insertRow(row);
        QString displayTitle = mod.title;
        if (displayTitle.isEmpty()) {
            if (const auto *lib = library.get(mod.id); lib && !lib->title.isEmpty())
                displayTitle = lib->title;
            else
                displayTitle = QStringLiteral("Workshop item ") + mod.id;
        }
        auto *titleItem = new QTableWidgetItem(displayTitle);
        titleItem->setData(Qt::UserRole, mod.id);
        titleItem->setData(Qt::UserRole + 1, mod.sourceCollectionId);
        serverModsView->setItem(row, 0, titleItem);

        const QString sourceText =
            mod.sourceCollectionId.isEmpty()
                ? QStringLiteral("Direct mod")
                : QStringLiteral("Collection: %1")
                      .arg(collectionNames.value(mod.sourceCollectionId, mod.sourceCollectionId));
        serverModsView->setItem(row, 1, new QTableWidgetItem(sourceText));

        const bool existsOnDisk = serverModFilesExist(*server, mod);
        QString statusText;
        if (const auto *qItem = queue.get(mod.id);
            qItem && (qItem->status == QStringLiteral("Downloading") ||
                      qItem->status == QStringLiteral("Verifying"))) {
            statusText = qItem->status;
            ++pendingCount;
        } else if (!existsOnDisk) {
            statusText = mod.installedAt > 0 ? QStringLiteral("Missing files")
                                             : QStringLiteral("New (not installed)");
            ++pendingCount;
        } else if (mod.remoteUpdated > 0 && mod.updated > 0 && mod.remoteUpdated > mod.updated) {
            statusText = QStringLiteral("Update available");
            ++updateCount;
            ++installedCount;
        } else {
            statusText = QStringLiteral("Installed");
            ++installedCount;
        }
        serverModsView->setItem(row, 2, new QTableWidgetItem(statusText));
        serverModsView->setItem(row, 3, new QTableWidgetItem(mod.id));
        serverModsView->setItem(
            row, 4, new QTableWidgetItem(QDir::toNativeSeparators(expectedServerModPath(*server, mod))));
    }

    serverNoteLabel->setText(
        QStringLiteral("%1 collection(s) · %2 total mod(s) · %3 installed · %4 update(s) available · %5 "
                       "not yet installed")
            .arg(server->collections.size())
            .arg(server->mods.size())
            .arg(installedCount)
            .arg(updateCount)
            .arg(pendingCount));
}

void Window::editServerDialog(const QString &serverId) {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    const ServerProfile *existing = findServerProfile(serverId);

    QDialog dialog(this);
    dialog.setWindowTitle(existing ? QStringLiteral("Edit server profile")
                                   : QStringLiteral("New server profile"));
    dialog.resize(620, 520);
    auto *mainLayout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(QStringLiteral("e.g., Barotrauma - Main Server or tModLoader - Calamity"));
    nameEdit->setClearButtonEnabled(true);
    if (existing)
        nameEdit->setText(existing->name);
    form->addRow(QStringLiteral("Server &name:"), nameEdit);

    auto *presetCombo = new QComboBox(&dialog);
    presetCombo->addItem(QStringLiteral("Custom Steam App ID"), QString());
    presetCombo->addItem(QStringLiteral("Barotrauma (602960)"), QStringLiteral("602960"));
    presetCombo->addItem(QStringLiteral("tModLoader / Terraria (1281930)"), QStringLiteral("1281930"));
    for (const auto &p : gameProfiles()) {
        if (p.id != QStringLiteral("602960") && p.id != QStringLiteral("1281930") &&
            p.id != QStringLiteral("427520"))
            presetCombo->addItem(QStringLiteral("%1 (%2)").arg(p.name, p.id), p.id);
    }
    form->addRow(QStringLiteral("&Game preset:"), presetCombo);

    auto *appIdEdit = new QLineEdit(&dialog);
    appIdEdit->setPlaceholderText(
        QStringLiteral("Steam App ID (e.g., 602960 for Barotrauma, 1281930 for tModLoader)"));
    appIdEdit->setClearButtonEnabled(true);
    if (existing)
        appIdEdit->setText(existing->appId);
    form->addRow(QStringLiteral("Steam &App ID:"), appIdEdit);

    auto *folderRow = new QHBoxLayout();
    auto *folderEdit = new QLineEdit(&dialog);
    folderEdit->setPlaceholderText(
        QStringLiteral("Server mods folder (e.g., C:\\Servers\\Barotrauma\\LocalMods)"));
    folderEdit->setClearButtonEnabled(true);
    if (existing)
        folderEdit->setText(QDir::toNativeSeparators(existing->modsFolder));
    auto *browseBtn = new QPushButton(QStringLiteral("Browse…"), &dialog);
    folderRow->addWidget(folderEdit, 1);
    folderRow->addWidget(browseBtn);
    form->addRow(QStringLiteral("Server &mods folder:"), folderRow);

    auto *formatCombo = new QComboBox(&dialog);
    formatCombo->addItem(QStringLiteral("Mod ID subfolders (<ModsFolder>/<WorkshopID>)"),
                         QStringLiteral("subfolders"));
    formatCombo->addItem(QStringLiteral("Game package format (.tmod, .gma, .vpk, .pak, etc.)"),
                         QStringLiteral("game_profile"));
    formatCombo->addItem(QStringLiteral("Titled subfolders (<ModsFolder>/<WorkshopID>_<Title>)"),
                         QStringLiteral("id_title"));
    if (existing)
        formatCombo->setCurrentIndex(qMax(0, formatCombo->findData(existing->installFormat)));
    form->addRow(QStringLiteral("&Install layout:"), formatCombo);

    auto *cleanRemovedCheck = new QCheckBox(
        QStringLiteral("Remove mod files from server folder when removed from Workshop collection"), &dialog);
    cleanRemovedCheck->setChecked(existing ? existing->cleanRemoved : false);
    form->addRow(QString(), cleanRemovedCheck);

    mainLayout->addLayout(form);

    auto *entriesGroup =
        new QGroupBox(QStringLiteral("Mods and Workshop collections for this server"), &dialog);
    auto *entriesLayout = new QVBoxLayout(entriesGroup);
    auto *entriesHint = new QLabel(
        QStringLiteral("Paste Steam Workshop collection links, mod links, or IDs (one per line). "
                       "Collections will automatically expand and stay updated when you sync the server."),
        entriesGroup);
    entriesHint->setWordWrap(true);
    entriesLayout->addWidget(entriesHint);

    auto *entriesEdit = new QPlainTextEdit(entriesGroup);
    entriesEdit->setPlaceholderText(
        QStringLiteral("https://steamcommunity.com/sharedfiles/filedetails/?id=...\n2954239072"));
    if (existing) {
        QStringList lines;
        for (const auto &col : existing->collections)
            lines << workshopUrl(col.id);
        for (const auto &modId : existing->directModIds)
            lines << workshopUrl(modId);
        entriesEdit->setPlainText(lines.join(QLatin1Char('\n')));
    }
    entriesLayout->addWidget(entriesEdit, 1);

    auto *entriesBtnRow = new QHBoxLayout();
    auto *pasteEntriesBtn = new QPushButton(QStringLiteral("Paste clipboard"), entriesGroup);
    auto *addBrowserPageBtn = new QPushButton(QStringLiteral("Add current Browser page"), entriesGroup);
    entriesBtnRow->addWidget(pasteEntriesBtn);
    entriesBtnRow->addWidget(addBrowserPageBtn);
    entriesBtnRow->addStretch(1);
    entriesLayout->addLayout(entriesBtnRow);
    mainLayout->addWidget(entriesGroup, 1);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    mainLayout->addWidget(buttons);

    if (existing) {
        const int presetIdx = presetCombo->findData(existing->appId);
        if (presetIdx >= 0)
            presetCombo->setCurrentIndex(presetIdx);
    }
    connect(presetCombo, &QComboBox::currentIndexChanged, &dialog,
            [presetCombo, appIdEdit, formatCombo](int index) {
                const QString id = presetCombo->itemData(index).toString();
                if (!id.isEmpty()) {
                    appIdEdit->setText(id);
                    if (id == QStringLiteral("1281930") || id == QStringLiteral("4000") ||
                        id == QStringLiteral("550") || id == QStringLiteral("211820"))
                        formatCombo->setCurrentIndex(
                            qMax(0, formatCombo->findData(QStringLiteral("game_profile"))));
                    else if (id == QStringLiteral("602960"))
                        formatCombo->setCurrentIndex(
                            qMax(0, formatCombo->findData(QStringLiteral("subfolders"))));
                }
            });
    connect(appIdEdit, &QLineEdit::textChanged, &dialog, [presetCombo](const QString &text) {
        const QSignalBlocker block(presetCombo);
        const int idx = presetCombo->findData(text.trimmed());
        presetCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    });
    connect(browseBtn, &QPushButton::clicked, &dialog, [&] {
        const QString chosen = QFileDialog::getExistingDirectory(
            &dialog, QStringLiteral("Select server mods folder"), folderEdit->text());
        if (!chosen.isEmpty())
            folderEdit->setText(QDir::toNativeSeparators(chosen));
    });
    connect(pasteEntriesBtn, &QPushButton::clicked, &dialog, [entriesEdit] {
        const QString clip = QApplication::clipboard()->text().trimmed();
        if (!clip.isEmpty())
            entriesEdit->appendPlainText(clip);
    });
    connect(addBrowserPageBtn, &QPushButton::clicked, &dialog, [this, entriesEdit] {
        if (web && ! parseIds(web->url().toString()).isEmpty())
            entriesEdit->appendPlainText(web->url().toString());
        else
            QMessageBox::information(this, QStringLiteral("Browser page"),
                                     QStringLiteral("Open a Workshop mod or collection in the Browser tab first."));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QString name = nameEdit->text().trimmed();
        QString appId = appIdEdit->text().trimmed();
        if (appId == QStringLiteral("1026340"))
            appId = QStringLiteral("602960");
        const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(folderEdit->text().trimmed()));
        if (name.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Server name"),
                                 QStringLiteral("Enter a name for this server profile."));
            nameEdit->setFocus();
            return;
        }
        if (!validId(appId)) {
            QMessageBox::warning(&dialog, QStringLiteral("Steam App ID"),
                                 QStringLiteral("Enter a positive numeric Steam App ID (for example, 602960 "
                                                "for Barotrauma or 1281930 for tModLoader)."));
            appIdEdit->setFocus();
            return;
        }
        if (!QDir::isAbsolutePath(folder) || !QDir().mkpath(folder)) {
            QMessageBox::warning(&dialog, QStringLiteral("Server mods folder"),
                                 QStringLiteral("Choose a valid, writable absolute folder for server mods."));
            folderEdit->setFocus();
            return;
        }
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString name = nameEdit->text().trimmed();
    const QString appId = normalizeWorkshopAppId(appIdEdit->text().trimmed());
    const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(folderEdit->text().trimmed()));
    const QString installFormat = formatCombo->currentData().toString();
    const bool cleanRemoved = cleanRemovedCheck->isChecked();
    const QStringList enteredIds = parseIds(entriesEdit->toPlainText());

    QString targetId = serverId;
    if (auto *s = findServerProfile(serverId)) {
        s->name = name;
        s->appId = appId;
        s->modsFolder = folder;
        s->installFormat = installFormat;
        s->cleanRemoved = cleanRemoved;
        // Remove collections or direct mods that were deleted from the text box
        for (int i = int(s->collections.size()) - 1; i >= 0; --i) {
            if (!enteredIds.contains(s->collections[i].id)) {
                const QString colId = s->collections[i].id;
                s->collections.removeAt(i);
                for (auto it = s->mods.begin(); it != s->mods.end();) {
                    if (it.value().sourceCollectionId == colId && !s->directModIds.contains(it.key()))
                        it = s->mods.erase(it);
                    else
                        ++it;
                }
            }
        }
        for (int i = int(s->directModIds.size()) - 1; i >= 0; --i) {
            const QString modId = s->directModIds[i];
            bool inCollection = false;
            for (const auto &col : s->collections)
                inCollection = inCollection || col.children.contains(modId);
            if (!enteredIds.contains(modId) && !inCollection) {
                s->directModIds.removeAt(i);
                s->mods.remove(modId);
            }
        }
    } else {
        ServerProfile newProfile;
        newProfile.id = QUuid::createUuid().toString(QUuid::Id128);
        newProfile.name = name;
        newProfile.appId = appId;
        newProfile.modsFolder = folder;
        newProfile.installFormat = installFormat;
        newProfile.cleanRemoved = cleanRemoved;
        targetId = newProfile.id;
        serverProfiles.append(newProfile);
    }

    engine.settings.setValue(QStringLiteral("selectedServerProfile"), targetId);
    saveServerProfiles();
    reloadServers();
    refresh();

    if (!enteredIds.isEmpty())
        addEntriesToServer(targetId, enteredIds, false);
}

void Window::deleteServerProfile() {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    const QString id = selectedServerId();
    const auto *server = findServerProfile(id);
    if (!server)
        return;
    if (QMessageBox::question(
            this, QStringLiteral("Delete server profile"),
            QStringLiteral("Remove the server profile '%1'?\nInstalled files in %2 will be kept on disk.")
                .arg(server->name, QDir::toNativeSeparators(server->modsFolder))) != QMessageBox::Yes)
        return;
    for (int i = 0; i < serverProfiles.size(); ++i)
        if (serverProfiles[i].id == id) {
            serverProfiles.removeAt(i);
            break;
        }
    saveServerProfiles();
    reloadServers();
    refresh();
}

void Window::setServerModsFolder() {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    auto *server = findServerProfile(selectedServerId());
    if (!server)
        return;
    const QString chosen = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select mods folder for %1").arg(server->name), server->modsFolder);
    if (chosen.isEmpty())
        return;
    const QString clean = QDir::cleanPath(chosen.trimmed());
    if (!QDir::isAbsolutePath(clean) || !QDir().mkpath(clean)) {
        QMessageBox::warning(this, QStringLiteral("Server mods folder"),
                             QStringLiteral("Choose a writable absolute folder."));
        return;
    }
    server->modsFolder = clean;
    saveServerProfiles();
    reloadServers();
    activity->setText(QStringLiteral("%1 mods folder set to %2")
                          .arg(server->name, QDir::toNativeSeparators(clean)));
}

void Window::openServerModsFolder() {
    const auto *server = findServerProfile(selectedServerId());
    if (!server)
        return;
    QDir().mkpath(server->modsFolder);
    openFolder(server->modsFolder);
}

void Window::addServerModsDialog() {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    const auto *server = findServerProfile(selectedServerId());
    if (!server)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add mods or collections to %1").arg(server->name));
    dialog.resize(580, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(
        QStringLiteral("Paste Steam Workshop collection links, mod links, or IDs (one per line). "
                       "Collections will automatically expand into all of their mods."),
        &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);

    auto *edit = new QPlainTextEdit(&dialog);
    edit->setPlaceholderText(
        QStringLiteral("https://steamcommunity.com/sharedfiles/filedetails/?id=...\n2954239072"));
    layout->addWidget(edit, 1);

    auto *helperRow = new QHBoxLayout();
    auto *pasteBtn = new QPushButton(QStringLiteral("Paste clipboard"), &dialog);
    auto *browserBtn = new QPushButton(QStringLiteral("Add current Browser page"), &dialog);
    helperRow->addWidget(pasteBtn);
    helperRow->addWidget(browserBtn);
    helperRow->addStretch(1);
    layout->addLayout(helperRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *addOnlyBtn = buttons->addButton(QStringLiteral("Add to server"), QDialogButtonBox::AcceptRole);
    auto *addAndSyncBtn =
        buttons->addButton(QStringLiteral("Add && install now"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);

    bool syncImmediately = false;
    connect(pasteBtn, &QPushButton::clicked, &dialog, [edit] {
        const QString clip = QApplication::clipboard()->text().trimmed();
        if (!clip.isEmpty())
            edit->appendPlainText(clip);
    });
    connect(browserBtn, &QPushButton::clicked, &dialog, [this, edit] {
        if (web && !parseIds(web->url().toString()).isEmpty())
            edit->appendPlainText(web->url().toString());
        else
            QMessageBox::information(
                this, QStringLiteral("Browser page"),
                QStringLiteral("Open a Workshop mod or collection in the Browser tab first."));
    });
    connect(addOnlyBtn, &QPushButton::clicked, &dialog, [&] {
        syncImmediately = false;
        dialog.accept();
    });
    connect(addAndSyncBtn, &QPushButton::clicked, &dialog, [&] {
        syncImmediately = true;
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;
    const QStringList ids = parseIds(edit->toPlainText());
    if (ids.isEmpty()) {
        activity->setText(QStringLiteral("Enter at least one valid Workshop link or ID."));
        return;
    }
    addEntriesToServer(server->id, ids, syncImmediately);
}

void Window::addEntriesToServer(const QString &serverId, const QStringList &rawIds, bool syncAfter) {
    if (rawIds.isEmpty())
        return;
    auto *server = findServerProfile(serverId);
    if (!server)
        return;
    diskBusy = true;
    refresh();
    activity->setText(QStringLiteral("Resolving Workshop items and collections for %1…").arg(server->name));

    engine.inspectWorkshop(rawIds, this, [this, serverId, rawIds, syncAfter](WorkshopEntries entries) {
        auto *s = findServerProfile(serverId);
        if (!s) {
            diskBusy = false;
            refresh();
            return;
        }
        QStringList childIdsToPreview;
        for (const auto &id : rawIds) {
            const auto entry = entries.value(id);
            if (entry.collection) {
                ServerCollectionEntry *existingCol = nullptr;
                for (auto &c : s->collections)
                    if (c.id == id) {
                        existingCol = &c;
                        break;
                    }
                if (!existingCol) {
                    ServerCollectionEntry c;
                    c.id = id;
                    c.title = entry.details.value(QStringLiteral("title")).toString(id);
                    c.children = entry.children;
                    s->collections.append(c);
                } else {
                    existingCol->title =
                        entry.details.value(QStringLiteral("title")).toString(existingCol->title);
                    existingCol->children = entry.children;
                }
                for (const auto &childId : entry.children) {
                    if (!s->mods.contains(childId)) {
                        ServerModRecord m;
                        m.id = childId;
                        m.sourceCollectionId = id;
                        s->mods.insert(childId, m);
                    } else if (s->mods[childId].sourceCollectionId.isEmpty()) {
                        s->mods[childId].sourceCollectionId = id;
                    }
                    childIdsToPreview << childId;
                }
            } else {
                if (!s->directModIds.contains(id))
                    s->directModIds << id;
                auto &m = s->mods[id];
                m.id = id;
                const QString resolvedTitle = entry.details.value(QStringLiteral("title")).toString();
                if (!resolvedTitle.isEmpty())
                    m.title = resolvedTitle;
                const qint64 remoteUpd = entry.details.value(QStringLiteral("time_updated")).toInteger();
                if (remoteUpd > 0)
                    m.remoteUpdated = remoteUpd;
            }
        }
        childIdsToPreview.removeDuplicates();
        if (childIdsToPreview.isEmpty()) {
            diskBusy = false;
            saveServerProfiles();
            reloadServers();
            refresh();
            activity->setText(QStringLiteral("Updated mod list for %1.").arg(s->name));
            if (syncAfter)
                syncServerProfile(serverId);
            return;
        }
        engine.preview(
            childIdsToPreview, this,
            [this, serverId, syncAfter](QHash<QString, QJsonObject> details, QString) {
                if (auto *srv = findServerProfile(serverId)) {
                    for (auto it = details.cbegin(); it != details.cend(); ++it) {
                        if (srv->mods.contains(it.key())) {
                            auto &m = srv->mods[it.key()];
                            const QString t = it.value().value(QStringLiteral("title")).toString();
                            if (!t.isEmpty())
                                m.title = t;
                            const qint64 upd = it.value().value(QStringLiteral("time_updated")).toInteger();
                            if (upd > 0)
                                m.remoteUpdated = upd;
                        }
                    }
                }
                diskBusy = false;
                saveServerProfiles();
                reloadServers();
                refresh();
                if (syncAfter)
                    syncServerProfile(serverId);
                else if (const auto *srv = findServerProfile(serverId))
                    activity->setText(
                        QStringLiteral("Added items to %1 (%2 total mods).")
                            .arg(srv->name)
                            .arg(srv->mods.size()));
            });
    });
}

void Window::removeSelectedServerMods() {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    auto *server = findServerProfile(selectedServerId());
    if (!server || !serverModsView)
        return;
    QStringList selectedModIds;
    QSet<QString> selectedCollections;
    for (const auto &idx : serverModsView->selectionModel()->selectedRows()) {
        const QString modId = serverModsView->item(idx.row(), 0)->data(Qt::UserRole).toString();
        const QString colId = serverModsView->item(idx.row(), 0)->data(Qt::UserRole + 1).toString();
        if (!modId.isEmpty())
            selectedModIds << modId;
        if (!colId.isEmpty())
            selectedCollections.insert(colId);
    }
    if (selectedModIds.isEmpty())
        return;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Remove from server"));
    box.setText(QStringLiteral("Remove %1 selected mod(s) from '%2'?")
                    .arg(selectedModIds.size())
                    .arg(server->name));
    box.setInformativeText(
        selectedCollections.isEmpty()
            ? QStringLiteral("Do you also want to delete their installed files from the server mods folder?")
            : QStringLiteral("Note: Some selected mods belong to a collection. Removing them will also "
                             "remove the collection link or detach those mods."));
    auto *deleteDiskBtn =
        box.addButton(QStringLiteral("Remove && delete server files"), QMessageBox::DestructiveRole);
    auto *removeProfileOnlyBtn =
        box.addButton(QStringLiteral("Remove from profile only"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != deleteDiskBtn && box.clickedButton() != removeProfileOnlyBtn)
        return;

    const bool deleteFromDisk = (box.clickedButton() == deleteDiskBtn);
    for (const auto &modId : selectedModIds) {
        if (deleteFromDisk && server->mods.contains(modId))
            removeServerModFiles(*server, server->mods.value(modId));
        server->directModIds.removeAll(modId);
        for (int i = int(server->collections.size()) - 1; i >= 0; --i) {
            server->collections[i].children.removeAll(modId);
            if (server->collections[i].children.isEmpty())
                server->collections.removeAt(i);
        }
        server->mods.remove(modId);
    }
    saveServerProfiles();
    reloadServers();
    refresh();
}

void Window::checkServerUpdates(const QString &serverId, bool syncAfter) {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    const QString targetId = serverId.isEmpty() ? selectedServerId() : serverId;
    auto *server = findServerProfile(targetId);
    if (!server)
        return;
    if (server->collections.isEmpty() && server->directModIds.isEmpty() && server->mods.isEmpty()) {
        activity->setText(
            QStringLiteral("Add Workshop mods or collections to %1 first.").arg(server->name));
        if (syncAfter && !pendingServerSyncIds.isEmpty())
            syncServerProfile(pendingServerSyncIds.takeFirst());
        return;
    }

    diskBusy = true;
    refresh();
    activity->setText(
        QStringLiteral("Checking Workshop collections and mod updates for %1…").arg(server->name));

    QStringList collectionIds;
    for (const auto &col : server->collections)
        collectionIds << col.id;

    auto inspectStep = [this, targetId, syncAfter] {
        auto *srv = findServerProfile(targetId);
        if (!srv) {
            diskBusy = false;
            refresh();
            return;
        }
        const QStringList allModIds = srv->mods.keys();
        if (allModIds.isEmpty()) {
            diskBusy = false;
            saveServerProfiles();
            reloadServers();
            refresh();
            activity->setText(QStringLiteral("No mods found in %1.").arg(srv->name));
            if (syncAfter && !pendingServerSyncIds.isEmpty())
                syncServerProfile(pendingServerSyncIds.takeFirst());
            return;
        }
        engine.preview(
            allModIds, this,
            [this, targetId, syncAfter](QHash<QString, QJsonObject> details, QString error) {
                auto *s = findServerProfile(targetId);
                if (!s) {
                    diskBusy = false;
                    refresh();
                    return;
                }
                QStringList toDownload;
                QStringList readyToInstall;
                int updatesFound = 0, newOrMissing = 0;

                for (auto it = s->mods.begin(); it != s->mods.end(); ++it) {
                    auto &mod = it.value();
                    const auto obj = details.value(mod.id);
                    const bool ok = obj.value(QStringLiteral("result")).toVariant().toInt() == 1;
                    QString resolvedAppId = normalizeWorkshopAppId(s->appId);
                    if (ok) {
                        mod.title = obj.value(QStringLiteral("title")).toString(mod.title);
                        mod.remoteUpdated =
                            obj.value(QStringLiteral("time_updated")).toInteger(mod.remoteUpdated);
                        const auto appVal = obj.value(QStringLiteral("consumer_app_id"));
                        const QString consumerApp =
                            appVal.isString() ? appVal.toString() : QString::number(appVal.toInteger());
                        if (validId(consumerApp))
                            resolvedAppId = consumerApp;
                    }

                    const bool existsInServer = serverModFilesExist(*s, mod);
                    const bool needsUpdate =
                        !existsInServer ||
                        (mod.remoteUpdated > 0 && mod.remoteUpdated > mod.updated);
                    if (!existsInServer)
                        ++newOrMissing;
                    else if (needsUpdate)
                        ++updatesFound;

                    if (syncAfter && needsUpdate) {
                        const auto *libItem = library.get(mod.id);
                        const bool libUpToDate =
                            libItem && !libItem->path.isEmpty() && QDir(libItem->path).exists() &&
                            (mod.remoteUpdated <= 0 || libItem->updated >= mod.remoteUpdated);
                        if (libUpToDate) {
                            readyToInstall << mod.id;
                        } else {
                            if (!queue.get(mod.id)) {
                                Item q;
                                q.id = mod.id;
                                q.appId = resolvedAppId;
                                q.manualAppId = true;
                                q.title = mod.title;
                                q.game = gameName(resolvedAppId);
                                q.status = QStringLiteral("Ready");
                                if (ok) {
                                    const auto sz = obj.value(QStringLiteral("file_size"));
                                    q.size = sz.isString() ? sz.toString().toLongLong() : sz.toInteger();
                                    q.updated = mod.remoteUpdated;
                                    q.fileUrl = obj.value(QStringLiteral("file_url")).toString();
                                    q.filename = obj.value(QStringLiteral("filename")).toString();
                                }
                                queue.add(q);
                            } else if (auto *q = queue.get(mod.id)) {
                                q->appId = resolvedAppId;
                                q->manualAppId = true;
                                if (!mod.title.isEmpty())
                                    q->title = mod.title;
                                q->game = gameName(resolvedAppId);
                                q->status = QStringLiteral("Ready");
                                q->error.clear();
                                if (ok) {
                                    const auto sz = obj.value(QStringLiteral("file_size"));
                                    q->size = sz.isString() ? sz.toString().toLongLong() : sz.toInteger();
                                    q->updated = mod.remoteUpdated;
                                    q->fileUrl = obj.value(QStringLiteral("file_url")).toString();
                                    q->filename = obj.value(QStringLiteral("filename")).toString();
                                }
                                queue.changed(mod.id);
                            }
                            toDownload << mod.id;
                        }
                    }
                }

                diskBusy = false;
                saveServerProfiles();
                engine.persist();
                reloadServers();
                refresh();

                if (!syncAfter) {
                    activity->setText(
                        error.isEmpty()
                            ? QStringLiteral("%1: %2 update(s) available · %3 new/missing mod(s).")
                                  .arg(s->name)
                                  .arg(updatesFound)
                                  .arg(newOrMissing)
                            : QStringLiteral("Update check warning: ") + error);
                    return;
                }

                activeServerInstallModIds = readyToInstall + toDownload;
                activeServerInstallModIds.removeDuplicates();

                if (activeServerInstallModIds.isEmpty()) {
                    activity->setText(
                        QStringLiteral("%1 is already up to date (%2 mods installed).")
                            .arg(s->name)
                            .arg(s->mods.size()));
                    appendLog(QStringLiteral("Server '%1' is up to date.").arg(s->name));
                    if (!pendingServerSyncIds.isEmpty())
                        syncServerProfile(pendingServerSyncIds.takeFirst());
                    return;
                }

                if (!toDownload.isEmpty()) {
                    activeServerSyncId = targetId;
                    appendLog(QStringLiteral("Downloading %1 new/updated mod(s) for server '%2'…")
                                  .arg(toDownload.size())
                                  .arg(s->name));
                    engine.start(toDownload);
                } else {
                    installServerModsNow(targetId, activeServerInstallModIds);
                }
            });
    };

    if (collectionIds.isEmpty()) {
        inspectStep();
        return;
    }

    engine.inspectWorkshop(
        collectionIds, this, [this, targetId, inspectStep](WorkshopEntries entries) {
            if (auto *srv = findServerProfile(targetId)) {
                QSet<QString> validCollectionMods;
                for (auto &col : srv->collections) {
                    const auto entry = entries.value(col.id);
                    if (entry.collection && !entry.children.isEmpty()) {
                        col.title = entry.details.value(QStringLiteral("title")).toString(col.title);
                        col.children = entry.children;
                    }
                    for (const auto &childId : col.children) {
                        validCollectionMods.insert(childId);
                        if (!srv->mods.contains(childId)) {
                            ServerModRecord m;
                            m.id = childId;
                            m.sourceCollectionId = col.id;
                            srv->mods.insert(childId, m);
                        }
                    }
                }
                if (srv->cleanRemoved) {
                    for (auto it = srv->mods.begin(); it != srv->mods.end();) {
                        const auto &m = it.value();
                        if (!m.sourceCollectionId.isEmpty() && !validCollectionMods.contains(m.id) &&
                            !srv->directModIds.contains(m.id)) {
                            removeServerModFiles(*srv, m);
                            it = srv->mods.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }
            inspectStep();
        });
}

void Window::syncServerProfile(const QString &serverId) {
    if (engine.busy || diskBusy || engine.resolving || serverId.isEmpty())
        return;
    checkServerUpdates(serverId, true);
}

void Window::syncAllServerProfiles() {
    if (engine.busy || diskBusy || engine.resolving || serverProfiles.isEmpty())
        return;
    pendingServerSyncIds.clear();
    for (const auto &s : serverProfiles)
        pendingServerSyncIds << s.id;
    const QString first = pendingServerSyncIds.takeFirst();
    syncServerProfile(first);
}

void Window::finishServerDownloadRun() {
    const QString serverId = activeServerSyncId;
    const QStringList modIds = activeServerInstallModIds;
    activeServerSyncId.clear();
    activeServerInstallModIds.clear();
    if (serverId.isEmpty() || modIds.isEmpty())
        return;
    installServerModsNow(serverId, modIds);
}

void Window::installServerModsNow(const QString &serverId, const QStringList &modIds) {
    const auto *server = findServerProfile(serverId);
    if (!server || modIds.isEmpty()) {
        if (!pendingServerSyncIds.isEmpty())
            syncServerProfile(pendingServerSyncIds.takeFirst());
        return;
    }

    struct InstallTask {
        ServerModRecord mod;
        Item sourceItem;
    };
    QList<InstallTask> tasks;
    QStringList missingDownloads;
    for (const auto &id : modIds) {
        if (!server->mods.contains(id))
            continue;
        const auto *libItem = library.get(id);
        if (!libItem || libItem->path.isEmpty() || !QDir(libItem->path).exists()) {
            missingDownloads << id;
            continue;
        }
        tasks.append({server->mods.value(id), *libItem});
    }

    if (tasks.isEmpty()) {
        reloadServers();
        refresh();
        if (!missingDownloads.isEmpty())
            activity->setText(QStringLiteral("Download failed for %1 mod(s); server files were not updated.")
                                  .arg(missingDownloads.size()));
        if (!pendingServerSyncIds.isEmpty())
            syncServerProfile(pendingServerSyncIds.takeFirst());
        return;
    }

    const ServerProfile snapshot = *server;
    diskBusy = true;
    refresh();
    activity->setText(QStringLiteral("Installing %1 mod(s) into %2…")
                          .arg(tasks.size())
                          .arg(server->name));

    auto *watcher = new QFutureWatcher<QList<ServerInstallOutcome>>(this);
    connect(watcher, &QFutureWatcher<QList<ServerInstallOutcome>>::finished, this,
            [this, watcher, serverId, missingDownloads] {
                const auto outcomes = watcher->result();
                watcher->deleteLater();
                diskBusy = false;

                int installed = 0;
                QStringList errors;
                for (const auto &id : missingDownloads)
                    errors << QStringLiteral("Mod %1 could not be downloaded.").arg(id);

                if (auto *srv = findServerProfile(serverId)) {
                    for (const auto &out : outcomes) {
                        if (!out.error.isEmpty()) {
                            errors << QStringLiteral("%1: %2").arg(out.modId, out.error);
                            continue;
                        }
                        if (srv->mods.contains(out.modId)) {
                            auto &m = srv->mods[out.modId];
                            m.installedPaths = out.installedPaths;
                            m.updated = out.updated > 0 ? out.updated : QDateTime::currentSecsSinceEpoch();
                            if (m.remoteUpdated <= 0)
                                m.remoteUpdated = m.updated;
                            m.installedAt = QDateTime::currentSecsSinceEpoch();
                            ++installed;
                        }
                    }
                    saveServerProfiles();
                    appendLog(QStringLiteral("Server '%1': installed/updated %2 mod(s) into %3.")
                                  .arg(srv->name)
                                  .arg(installed)
                                  .arg(QDir::toNativeSeparators(srv->modsFolder)));
                    activity->setText(
                        errors.isEmpty()
                            ? QStringLiteral("%1 updated: %2 mod(s) installed to server folder.")
                                  .arg(srv->name)
                                  .arg(installed)
                            : QStringLiteral("%1: %2 installed, %3 failed (see Activity log).")
                                  .arg(srv->name)
                                  .arg(installed)
                                  .arg(errors.size()));
                }
                if (!errors.isEmpty())
                    appendLog(errors.join(QLatin1Char('\n')));

                reloadServers();
                refresh();
                if (!pendingServerSyncIds.isEmpty())
                    syncServerProfile(pendingServerSyncIds.takeFirst());
            });

    watcher->setFuture(QtConcurrent::run([snapshot, tasks] {
        QList<ServerInstallOutcome> results;
        for (const auto &t : tasks)
            results.append(installOneServerMod(snapshot, t.mod, t.sourceItem));
        return results;
    }));
}

void Window::serverContextMenu(const QPoint &pos) {
    const auto *server = findServerProfile(selectedServerId());
    if (!server || !serverModsView)
        return;
    const auto index = serverModsView->indexAt(pos);
    if (!index.isValid())
        return;
    if (!serverModsView->selectionModel()->isRowSelected(index.row(), {}))
        serverModsView->selectRow(index.row());

    QStringList modIds;
    for (const auto &rowIdx : serverModsView->selectionModel()->selectedRows()) {
        const QString id = serverModsView->item(rowIdx.row(), 0)->data(Qt::UserRole).toString();
        if (!id.isEmpty())
            modIds << id;
    }
    if (modIds.isEmpty())
        return;

    const QString firstId = modIds.first();
    const auto firstMod = server->mods.value(firstId);
    const QString path = expectedServerModPath(*server, firstMod);
    const bool idle = !engine.busy && !diskBusy && engine.resolving == 0;

    QMenu menu(this);
    auto *openAct = menu.addAction(QStringLiteral("Open in server folder"), this, [this, path, server] {
        if (QFileInfo::exists(path))
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
        else
            openFolder(server->modsFolder);
    });
    menu.setDefaultAction(openAct);
    menu.addAction(QStringLiteral("Open Workshop page"), this,
                   [this, firstId] { navigate(QUrl(workshopUrl(firstId))); });
    menu.addAction(QStringLiteral("Copy Workshop link(s)"), this, [modIds] {
        QStringList links;
        for (const auto &id : modIds)
            links << workshopUrl(id);
        QApplication::clipboard()->setText(links.join(QLatin1Char('\n')));
    });
    menu.addSeparator();
    auto *syncSelectedAct = menu.addAction(
        QStringLiteral("Download && install selected on server"), this, [this, serverId = server->id, modIds] {
            auto *srv = findServerProfile(serverId);
            if (!srv || engine.busy || diskBusy)
                return;
            for (const auto &id : modIds)
                if (srv->mods.contains(id))
                    srv->mods[id].updated = 0; // Force re-sync of selected
            saveServerProfiles();
            syncServerProfile(serverId);
        });
    syncSelectedAct->setEnabled(idle);
    menu.addSeparator();
    auto *removeAct =
        menu.addAction(QStringLiteral("Remove from server…"), this, &Window::removeSelectedServerMods);
    removeAct->setEnabled(idle);
    menu.exec(serverModsView->viewport()->mapToGlobal(pos));
}