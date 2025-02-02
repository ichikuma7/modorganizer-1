/*
Copyright (C) 2012 Sebastian Herbord. All rights reserved.

This file is part of Mod Organizer.

Mod Organizer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Mod Organizer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Mod Organizer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pluginlist.h"
#include "settings.h"
#include "scopeguard.h"
#include "modinfo.h"
#include "modlist.h"
#include "viewmarkingscrollbar.h"
#include <utility.h>
#include <iplugingame.h>
#include <espfile.h>
#include <report.h>
#include <windows_error.h>
#include <safewritefile.h>
#include <gameplugins.h>

#include <QtDebug>
#include <QMessageBox>
#include <QMimeData>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextCodec>
#include <QFileInfo>
#include <QListWidgetItem>
#include <QRegularExpression>
#include <QString>
#include <QApplication>
#include <QKeyEvent>
#include <QSortFilterProxyModel>

#include <ctime>
#include <algorithm>
#include <stdexcept>


using namespace MOBase;
using namespace MOShared;


static bool ByName(const PluginList::ESPInfo& LHS, const PluginList::ESPInfo& RHS) {
  return LHS.m_Name.toUpper() < RHS.m_Name.toUpper();
}

static bool ByPriority(const PluginList::ESPInfo& LHS, const PluginList::ESPInfo& RHS) {
  if (LHS.m_IsMaster && !RHS.m_IsMaster) {
    return true;
  } else if (!LHS.m_IsMaster && RHS.m_IsMaster) {
    return false;
  } else {
    return LHS.m_Priority < RHS.m_Priority;
  }
}

static bool ByDate(const PluginList::ESPInfo& LHS, const PluginList::ESPInfo& RHS) {
  return QFileInfo(LHS.m_FullPath).lastModified() < QFileInfo(RHS.m_FullPath).lastModified();
}

static QString TruncateString(const QString& text) {
  QString new_text = text;
  if (new_text.length() > 1024) {
    new_text.truncate(1024);
    new_text += "...";
  }
  return new_text;
}

PluginList::PluginList(QObject *parent)
  : QAbstractItemModel(parent)
  , m_FontMetrics(QFont())
{
  connect(this, SIGNAL(writePluginsList()), this, SLOT(generatePluginIndexes()));
  m_LastCheck.start();
}

PluginList::~PluginList()
{
  m_Refreshed.disconnect_all_slots();
  m_PluginMoved.disconnect_all_slots();
  m_PluginStateChanged.disconnect_all_slots();
}

QString PluginList::getColumnName(int column)
{
  switch (column) {
    case COL_NAME:     return tr("Name");
    case COL_PRIORITY: return tr("Priority");
    case COL_MODINDEX: return tr("Mod Index");
    case COL_FLAGS:    return tr("Flags");
    default: return tr("unknown");
  }
}


QString PluginList::getColumnToolTip(int column)
{
  switch (column) {
    case COL_NAME:     return tr("Name of the plugin");
    case COL_FLAGS:    return tr("Emblems to highlight things that might require attention.");
    case COL_PRIORITY: return tr("Load priority of plugins. The higher, the more \"important\" it is and thus "
                                 "overwrites data from plugins with lower priority.");
    case COL_MODINDEX: return tr("Determines the formids of objects originating from this mods.");
    default: return tr("unknown");
  }
}

void PluginList::highlightPlugins(const QItemSelectionModel *selection, const MOShared::DirectoryEntry &directoryEntry, const Profile &profile)
{
  for (auto &esp : m_ESPs) {
    esp.m_ModSelected = false;
  }
  for (QModelIndex idx : selection->selectedRows(ModList::COL_PRIORITY)) {
    int modIndex = idx.data(Qt::UserRole + 1).toInt();
    if (modIndex == UINT_MAX)
      continue;

    ModInfo::Ptr selectedMod = ModInfo::getByIndex(modIndex);
    if (!selectedMod.isNull() && profile.modEnabled(modIndex)) {
      QDir dir(selectedMod->absolutePath());
      QStringList plugins = dir.entryList(QStringList() << "*.esp" << "*.esm" << "*.esl");
      MOShared::FilesOrigin origin = directoryEntry.getOriginByName(selectedMod->internalName().toStdWString());
      if (plugins.size() > 0) {
        for (auto plugin : plugins) {
          MOShared::FileEntry::Ptr file = directoryEntry.findFile(plugin.toStdWString());
          if (file && file->getOrigin() != origin.getID()) {
            const std::vector<std::pair<int, std::pair<std::wstring, int>>> alternatives = file->getAlternatives();
            if (std::find_if(alternatives.begin(), alternatives.end(), [&](const std::pair<int, std::pair<std::wstring, int>>& element) { return element.first == origin.getID(); }) == alternatives.end())
              continue;
          }
          std::map<QString, int>::iterator iter = m_ESPsByName.find(plugin.toLower());
          if (iter != m_ESPsByName.end()) {
            m_ESPs[iter->second].m_ModSelected = true;
          }
        }
      }
    }
  }
  emit dataChanged(this->index(0, 0), this->index(static_cast<int>(m_ESPs.size()) - 1, this->columnCount() - 1));
}

void PluginList::refresh(const QString &profileName
                         , const DirectoryEntry &baseDirectory
                         , const QString &lockedOrderFile
                         , bool force)
{
  if (force) {
    m_ESPs.clear();
    m_ESPsByName.clear();
    m_ESPsByPriority.clear();
  }

  ChangeBracket<PluginList> layoutChange(this);

  QStringList primaryPlugins = m_GamePlugin->primaryPlugins();
  bool lightPluginsAreSupported = m_GamePlugin->feature<GamePlugins>()->lightPluginsAreSupported();

  m_CurrentProfile = profileName;

  QStringList availablePlugins;

  std::vector<FileEntry::Ptr> files = baseDirectory.getFiles();
  for (FileEntry::Ptr current : files) {
    if (current.get() == nullptr) {
      continue;
    }
    QString filename = ToQString(current->getName());

    QString extension = filename.right(3).toLower();

    if ((extension == "esp") || (extension == "esm") || (extension == "esl")) {

      availablePlugins.append(filename.toLower());

      if (m_ESPsByName.find(filename.toLower()) != m_ESPsByName.end()) {
        continue;
      }

      bool forceEnabled = Settings::instance().game().forceEnableCoreFiles() &&
        primaryPlugins.contains(filename, Qt::CaseInsensitive);
        //(std::find(primaryPlugins.begin(), primaryPlugins.end(), filename.toLower()) != primaryPlugins.end());

      bool archive = false;
      try {
        FilesOrigin &origin = baseDirectory.getOriginByID(current->getOrigin(archive));

        //name without extension
        QString baseName = QFileInfo(filename).baseName();

        QString iniPath = baseName + ".ini";
        bool hasIni = baseDirectory.findFile(ToWString(iniPath)).get() != nullptr;
        std::set<QString> loadedArchives;
        QString candidateName;
        for (FileEntry::Ptr archiveCandidate : files) {
          candidateName = ToQString(archiveCandidate->getName());
          if (candidateName.startsWith(baseName, Qt::CaseInsensitive) &&
             (candidateName.endsWith(".bsa", Qt::CaseInsensitive) ||
              candidateName.endsWith(".ba2", Qt::CaseInsensitive))) {
            loadedArchives.insert(candidateName);
          }
        }

        QString originName = ToQString(origin.getName());
        unsigned int modIndex = ModInfo::getIndex(originName);
        if (modIndex != UINT_MAX) {
          ModInfo::Ptr modInfo = ModInfo::getByIndex(modIndex);
          originName = modInfo->name();
        }

        m_ESPs.push_back(ESPInfo(filename, forceEnabled, originName, ToQString(current->getFullPath()), hasIni, loadedArchives, lightPluginsAreSupported));
        m_ESPs.rbegin()->m_Priority = -1;
      } catch (const std::exception &e) {
        reportError(tr("failed to update esp info for file %1 (source id: %2), error: %3").arg(filename).arg(current->getOrigin(archive)).arg(e.what()));
      }
    }
  }

  for (const auto &espName : m_ESPsByName) {
    if (!availablePlugins.contains(espName.first)) {
      m_ESPs[espName.second].m_Name = "";
    }
  }

  m_ESPs.erase(std::remove_if(m_ESPs.begin(), m_ESPs.end(),
                              [](const ESPInfo &info) -> bool {
                                return info.m_Name.isEmpty();
                              }),
               m_ESPs.end());

  fixPriorities();

  // functions in GamePlugins will use the IPluginList interface of this, so
  // indices need to work. priority will be off however
  updateIndices();

  GamePlugins *gamePlugins = m_GamePlugin->feature<GamePlugins>();
  gamePlugins->readPluginLists(this);

  testMasters();

  updateIndices();

  readLockedOrderFrom(lockedOrderFile);

  layoutChange.finish();

  refreshLoadOrder();
  emit dataChanged(this->index(0, 0),
                   this->index(static_cast<int>(m_ESPs.size()), columnCount()));

  m_Refreshed();
}

void PluginList::fixPriorities()
{
  std::vector<std::pair<int, int>> espPrios;

  for (int i = 0; i < m_ESPs.size(); ++i) {
    int prio = m_ESPs[i].m_Priority;
    if (prio == -1) {
      prio = INT_MAX;
    }
    espPrios.push_back(std::make_pair(prio, i));
  }

  std::sort(espPrios.begin(), espPrios.end(),
            [](const std::pair<int, int> &lhs, const std::pair<int, int> &rhs) {
              return lhs.first < rhs.first;
            });

  for (int i = 0; i < espPrios.size(); ++i) {
    m_ESPs[espPrios[i].second].m_Priority = i;
  }
}

void PluginList::enableESP(const QString &name, bool enable)
{
  std::map<QString, int>::iterator iter = m_ESPsByName.find(name.toLower());

  if (iter != m_ESPsByName.end()) {
    m_ESPs[iter->second].m_Enabled =
        enable | m_ESPs[iter->second].m_ForceEnabled;

    emit writePluginsList();
  } else {
    reportError(tr("Plugin not found: %1").arg(qUtf8Printable(name)));
  }
}

int PluginList::findPluginByPriority(int priority)
{
  for (int i = 0; i < m_ESPs.size(); i++ ) {
    if (m_ESPs[i].m_Priority == priority) {
      return i;
    }
  }
  log::error("No plugin with priority {}", priority);
  return -1;
}

void PluginList::enableSelected(const QItemSelectionModel *selectionModel)
{
  if (selectionModel->hasSelection()) {
    bool dirty = false;
    for (auto row : selectionModel->selectedRows(COL_PRIORITY)) {
      int rowIndex = findPluginByPriority(row.data().toInt());
      if (!m_ESPs[rowIndex].m_Enabled) {
        m_ESPs[rowIndex].m_Enabled = true;
        dirty = true;
      }
    }
    if (dirty) emit writePluginsList();
  }
}

void PluginList::disableSelected(const QItemSelectionModel *selectionModel)
{
  if (selectionModel->hasSelection()) {
    bool dirty = false;
    for (auto row : selectionModel->selectedRows(COL_PRIORITY)) {
      int rowIndex = findPluginByPriority(row.data().toInt());
      if (!m_ESPs[rowIndex].m_ForceEnabled && m_ESPs[rowIndex].m_Enabled) {
        m_ESPs[rowIndex].m_Enabled = false;
        dirty = true;
      }
    }
    if (dirty) emit writePluginsList();
  }
}


void PluginList::enableAll()
{
  if (QMessageBox::question(nullptr, tr("Confirm"), tr("Really enable all plugins?"),
                            QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
    for (ESPInfo &info : m_ESPs) {
      info.m_Enabled = true;
    }
    emit writePluginsList();
  }
}


void PluginList::disableAll()
{
  if (QMessageBox::question(nullptr, tr("Confirm"), tr("Really disable all plugins?"),
                            QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
    for (ESPInfo &info : m_ESPs) {
      if (!info.m_ForceEnabled) {
        info.m_Enabled = false;
      }
    }
    emit writePluginsList();
  }
}

void PluginList::sendToPriority(const QItemSelectionModel *selectionModel, int newPriority)
{
  if (selectionModel->hasSelection()) {
    std::vector<int> pluginsToMove;
    for (auto row: selectionModel->selectedRows(COL_PRIORITY)) {
      int rowIndex = findPluginByPriority(row.data().toInt());
      if (!m_ESPs[rowIndex].m_ForceEnabled) {
        pluginsToMove.push_back(rowIndex);
      }
    }
    if (pluginsToMove.size()) {
      changePluginPriority(pluginsToMove, newPriority);
    }
  }
}

bool PluginList::isEnabled(const QString &name)
{
  std::map<QString, int>::iterator iter = m_ESPsByName.find(name.toLower());

  if (iter != m_ESPsByName.end()) {
    return m_ESPs[iter->second].m_Enabled;
  } else {
    return false;
  }
}

void PluginList::clearInformation(const QString &name)
{
  std::map<QString, int>::iterator iter = m_ESPsByName.find(name.toLower());

  if (iter != m_ESPsByName.end()) {
    m_AdditionalInfo[name.toLower()].m_Messages.clear();
  }
}

void PluginList::clearAdditionalInformation()
{
  m_AdditionalInfo.clear();
}

void PluginList::addInformation(const QString &name, const QString &message)
{
  std::map<QString, int>::iterator iter = m_ESPsByName.find(name.toLower());

  if (iter != m_ESPsByName.end()) {
    m_AdditionalInfo[name.toLower()].m_Messages.append(message);
  } else {
    log::warn("failed to associate message for \"{}\"", name);
  }
}

bool PluginList::isEnabled(int index)
{
  return m_ESPs.at(index).m_Enabled;
}

void PluginList::readLockedOrderFrom(const QString &fileName)
{
  m_LockedOrder.clear();

  QFile file(fileName);
  if (!file.exists()) {
    // no locked load order, that's ok
    return;
  }

  file.open(QIODevice::ReadOnly);

  int lineNumber = 0;
  while (!file.atEnd()) {
    ++lineNumber;
    QByteArray line = file.readLine();
    if ((line.size() > 0) && (line.at(0) != '#')) {
      QList<QByteArray> fields = line.split('|');
      if (fields.count() == 2) {
        int priority = fields.at(1).trimmed().toInt();
        QString name = QString::fromUtf8(fields.at(0));
        // Avoid locking a force-enabled plugin
        if (!m_ESPs[m_ESPsByName.at(name)].m_ForceEnabled) {
          // Is this an open and unclaimed priority?
          if (m_ESPs[m_ESPsByPriority.at(priority)].m_ForceEnabled ||
             std::find_if(m_LockedOrder.begin(), m_LockedOrder.end(), [&](const std::pair<QString, int> &a) { return a.second == priority; }) != m_LockedOrder.end()) {
            // Attempt to find a priority but step over force-enabled plugins and already-set locks
            int calcPriority = priority;
            do {
              ++calcPriority;
            } while (calcPriority < m_ESPsByPriority.size() || (m_ESPs[m_ESPsByPriority.at(calcPriority)].m_ForceEnabled &&
                     std::find_if(m_LockedOrder.begin(), m_LockedOrder.end(), [&](const std::pair<QString, int> &a) { return a.second == calcPriority; }) != m_LockedOrder.end()));
            // If we have a match, we can reassign the priority...
            if (calcPriority < m_ESPsByPriority.size())
              m_LockedOrder[name] = calcPriority;
          } else {
            m_LockedOrder[name] = priority;
          }
        }
      } else {
        log::error("locked order file: invalid line #{} '{}'", lineNumber, QString::fromUtf8(line));
        reportError(tr("The file containing locked plugin indices is broken"));
        break;
      }
    }
  }
  file.close();
}

void PluginList::writeLockedOrder(const QString &fileName) const
{
  SafeWriteFile file(fileName);

  file->resize(0);
  file->write(QString("# This file was automatically generated by Mod Organizer.\r\n").toUtf8());
  for (auto iter = m_LockedOrder.begin(); iter != m_LockedOrder.end(); ++iter) {
    file->write(QString("%1|%2\r\n").arg(iter->first).arg(iter->second).toUtf8());
  }
  file.commit();
  log::debug("{} saved", QDir::toNativeSeparators(fileName));
}


void PluginList::saveTo(const QString &lockedOrderFileName
                        , const QString& deleterFileName
                        , bool hideUnchecked) const
{
  GamePlugins *gamePlugins = m_GamePlugin->feature<GamePlugins>();
  gamePlugins->writePluginLists(this);

  writeLockedOrder(lockedOrderFileName);

  if (hideUnchecked) {
    SafeWriteFile deleterFile(deleterFileName);
    deleterFile->write(QString("# This file was automatically generated by Mod Organizer.\r\n").toUtf8());

    for (size_t i = 0; i < m_ESPs.size(); ++i) {
      int priority = m_ESPsByPriority[i];
      if (!m_ESPs[priority].m_Enabled) {
        deleterFile->write(m_ESPs[priority].m_Name.toUtf8());
        deleterFile->write("\r\n");
      }
    }
    if (deleterFile.commitIfDifferent(m_LastSaveHash[deleterFileName])) {
      log::debug("{} saved", QDir::toNativeSeparators(deleterFileName));
    }
  } else if (QFile::exists(deleterFileName)) {
    shellDelete(QStringList() << deleterFileName);
  }
}


bool PluginList::saveLoadOrder(DirectoryEntry &directoryStructure)
{
  if (m_GamePlugin->loadOrderMechanism() != IPluginGame::LoadOrderMechanism::FileTime) {
    // nothing to do
    return true;
  }

  log::debug("setting file times on esps");

  for (ESPInfo &esp : m_ESPs) {
    std::wstring espName = ToWString(esp.m_Name);
    const FileEntry::Ptr fileEntry = directoryStructure.findFile(espName);
    if (fileEntry.get() != nullptr) {
      QString fileName;
      bool archive = false;
      int originid = fileEntry->getOrigin(archive);
      fileName = QString("%1\\%2").arg(QDir::toNativeSeparators(ToQString(directoryStructure.getOriginByID(originid).getPath()))).arg(esp.m_Name);

      HANDLE file = ::CreateFile(ToWString(fileName).c_str(), GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        if (::GetLastError() == ERROR_SHARING_VIOLATION) {
          // file is locked, probably the game is running
          return false;
        } else {
          throw windows_error(QObject::tr("failed to access %1").arg(fileName).toUtf8().constData());
        }
      }

      ULONGLONG temp = 0;
      temp = (145731ULL + esp.m_Priority) * 24 * 60 * 60 * 10000000ULL;

      FILETIME newWriteTime;

      newWriteTime.dwLowDateTime  = (DWORD)(temp & 0xFFFFFFFF);
      newWriteTime.dwHighDateTime = (DWORD)(temp >> 32);
      esp.m_Time = newWriteTime;
      fileEntry->setFileTime(newWriteTime);
      if (!::SetFileTime(file, nullptr, nullptr, &newWriteTime)) {
        throw windows_error(QObject::tr("failed to set file time %1").arg(fileName).toUtf8().constData());
      }

      CloseHandle(file);
    }
  }
  return true;
}

int PluginList::enabledCount() const
{
  int enabled = 0;
  for (const auto &info : m_ESPs) {
    if (info.m_Enabled) {
      ++enabled;
    }
  }
  return enabled;
}

QString PluginList::getIndexPriority(int index) const
{
  return m_ESPs[index].m_Index;
}

bool PluginList::isESPLocked(int index) const
{
  return m_LockedOrder.find(m_ESPs.at(index).m_Name.toLower()) != m_LockedOrder.end();
}

void PluginList::lockESPIndex(int index, bool lock)
{
  if (lock) {
    if (!m_ESPs.at(index).m_ForceEnabled)
      m_LockedOrder[getName(index).toLower()] = m_ESPs.at(index).m_LoadOrder;
    else
      return;
  } else {
    auto iter = m_LockedOrder.find(getName(index).toLower());
    if (iter != m_LockedOrder.end()) {
      m_LockedOrder.erase(iter);
    }
  }
  emit writePluginsList();
}

void PluginList::syncLoadOrder()
{
  int loadOrder = 0;
  for (unsigned int i = 0; i < m_ESPs.size(); ++i) {
    int index = m_ESPsByPriority[i];

    if (m_ESPs[index].m_Enabled) {
      m_ESPs[index].m_LoadOrder = loadOrder++;
    } else {
      m_ESPs[index].m_LoadOrder = -1;
    }
  }
}

void PluginList::refreshLoadOrder()
{
  ChangeBracket<PluginList> layoutChange(this);
  syncLoadOrder();
  // set priorities according to locked load order
  std::map<int, QString> lockedLoadOrder;
  std::for_each(m_LockedOrder.begin(), m_LockedOrder.end(),
                [&lockedLoadOrder] (const std::pair<QString, int> &ele) {
    lockedLoadOrder[ele.second] = ele.first; });

  int targetPrio = 0;
  bool savePluginsList = false;
  // this is guaranteed to iterate from lowest key (load order) to highest
  for (auto iter = lockedLoadOrder.begin(); iter != lockedLoadOrder.end(); ++iter) {
    auto nameIter = m_ESPsByName.find(iter->second.toLower());
    if (nameIter != m_ESPsByName.end()) {
      // locked esp exists

      // find the location to insert at
      while ((targetPrio < static_cast<int>(m_ESPs.size() - 1)) &&
             (m_ESPs[m_ESPsByPriority[targetPrio]].m_LoadOrder < iter->first)) {
        ++targetPrio;
      }

      if (static_cast<size_t>(targetPrio) >= m_ESPs.size()) {
        continue;
      }

      int temp = targetPrio;
      int index = nameIter->second;
      if (m_ESPs[index].m_Priority != temp) {
        setPluginPriority(index, temp);
        m_ESPs[index].m_LoadOrder = iter->first;
        syncLoadOrder();
        savePluginsList = true;
      }
    }
  }
  if (savePluginsList) {
    emit writePluginsList();
  }
}

void PluginList::disconnectSlots() {
  m_PluginMoved.disconnect_all_slots();
  m_Refreshed.disconnect_all_slots();
  m_PluginStateChanged.disconnect_all_slots();
}

int PluginList::timeElapsedSinceLastChecked() const
{
  return m_LastCheck.elapsed();
}

QStringList PluginList::pluginNames() const
{
  QStringList result;

  for (const ESPInfo &info : m_ESPs) {
    result.append(info.m_Name);
  }

  return result;
}

IPluginList::PluginStates PluginList::state(const QString &name) const
{
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter == m_ESPsByName.end()) {
    return IPluginList::STATE_MISSING;
  } else {
    return m_ESPs[iter->second].m_Enabled ? IPluginList::STATE_ACTIVE : IPluginList::STATE_INACTIVE;
  }
}

void PluginList::setState(const QString &name, PluginStates state) {
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter != m_ESPsByName.end()) {
    m_ESPs[iter->second].m_Enabled = (state == IPluginList::STATE_ACTIVE) ||
                                     m_ESPs[iter->second].m_ForceEnabled;
  } else {
    log::warn("Plugin not found: {}", name);
  }
}

void PluginList::setLoadOrder(const QStringList &pluginList)
{
  for (ESPInfo &info : m_ESPs) {
    info.m_Priority = -1;
  }
  int maxPriority = 0;
  for (const QString &plugin : pluginList) {
    auto iter = m_ESPsByName.find(plugin.toLower());
    if (iter !=m_ESPsByName.end()) {
      m_ESPs[iter->second].m_Priority = maxPriority++;
    }
  }

  // use old priorities
  for (ESPInfo &info : m_ESPs) {
    if (info.m_Priority == -1) {
      info.m_Priority = maxPriority++;
    }
  }
  updateIndices();
}

int PluginList::priority(const QString &name) const
{
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter == m_ESPsByName.end()) {
    return -1;
  } else {
    return m_ESPs[iter->second].m_Priority;
  }
}

int PluginList::loadOrder(const QString &name) const
{
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter == m_ESPsByName.end()) {
    return -1;
  } else {
    return m_ESPs[iter->second].m_LoadOrder;
  }
}

bool PluginList::isMaster(const QString &name) const
{
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter == m_ESPsByName.end()) {
    return false;
  } else {
    return m_ESPs[iter->second].m_IsMaster;
  }
}

bool PluginList::isLight(const QString &name) const
{
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter == m_ESPsByName.end()) {
    return false;
  } else {
    return m_ESPs[iter->second].m_IsLight;
  }
}

bool PluginList::isLightFlagged(const QString &name) const
{
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter == m_ESPsByName.end()) {
    return false;
  } else {
    return m_ESPs[iter->second].m_IsLightFlagged;
  }
}

QStringList PluginList::masters(const QString &name) const
{
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter == m_ESPsByName.end()) {
    return QStringList();
  } else {
    QStringList result;
    for (const QString &master : m_ESPs[iter->second].m_Masters) {
      result.append(master);
    }
    return result;
  }
}

QString PluginList::origin(const QString &name) const
{
  auto iter = m_ESPsByName.find(name.toLower());
  if (iter == m_ESPsByName.end()) {
    return QString();
  } else {
    return m_ESPs[iter->second].m_OriginName;
  }
}

bool PluginList::onPluginStateChanged(const std::function<void (const QString &, PluginStates)> &func)
{
  auto conn = m_PluginStateChanged.connect(func);
  return conn.connected();
}

bool PluginList::onRefreshed(const std::function<void ()> &callback)
{
  auto conn = m_Refreshed.connect(callback);
  return conn.connected();
}


bool PluginList::onPluginMoved(const std::function<void (const QString &, int, int)> &func)
{
  auto conn = m_PluginMoved.connect(func);
  return conn.connected();
}


void PluginList::updateIndices()
{
  m_ESPsByName.clear();
  m_ESPsByPriority.clear();
  m_ESPsByPriority.resize(m_ESPs.size());
  for (unsigned int i = 0; i < m_ESPs.size(); ++i) {
    if (m_ESPs[i].m_Priority < 0) {
      continue;
    }
    if (m_ESPs[i].m_Priority >= static_cast<int>(m_ESPs.size())) {
      log::error("invalid plugin priority: {}", m_ESPs[i].m_Priority);
      continue;
    }
    m_ESPsByName[m_ESPs[i].m_Name.toLower()] = i;
    m_ESPsByPriority.at(static_cast<size_t>(m_ESPs[i].m_Priority)) = i;
  }

  generatePluginIndexes();
}

void PluginList::generatePluginIndexes()
{
  int numESLs = 0;
  int numSkipped = 0;
  bool lightPluginsSupported = m_GamePlugin->feature<GamePlugins>()->lightPluginsAreSupported();
  for (int l = 0; l < m_ESPs.size(); ++l) {
    int i = m_ESPsByPriority.at(l);
    if (!m_ESPs[i].m_Enabled) {
      m_ESPs[i].m_Index = QString();
      ++numSkipped;
      continue;
    }
    if (lightPluginsSupported && (m_ESPs[i].m_IsLight || m_ESPs[i].m_IsLightFlagged)) {
      int ESLpos = 254 + ((numESLs + 1) / 4096);
      m_ESPs[i].m_Index = QString("%1:%2").arg(ESLpos, 2, 16, QChar('0')).arg((numESLs) % 4096, 3, 16, QChar('0')).toUpper();
      ++numESLs;
    } else {
      m_ESPs[i].m_Index = QString("%1").arg(l - numESLs - numSkipped, 2, 16, QChar('0')).toUpper();
    }
  }
  emit esplist_changed();
}


int PluginList::rowCount(const QModelIndex &parent) const
{
  if (!parent.isValid()) {
    return static_cast<int>(m_ESPs.size());
  } else {
    return 0;
  }
}

int PluginList::columnCount(const QModelIndex &) const
{
  return COL_LASTCOLUMN + 1;
}


void PluginList::testMasters()
{
//  emit layoutAboutToBeChanged();

  std::set<QString> enabledMasters;
  for (const auto& iter: m_ESPs) {
    if (iter.m_Enabled) {
      enabledMasters.insert(iter.m_Name.toLower());
    }
  }

  for (auto& iter: m_ESPs) {
    iter.m_MasterUnset.clear();
    if (iter.m_Enabled) {
      for (const auto& master: iter.m_Masters) {
        if (enabledMasters.find(master.toLower()) == enabledMasters.end()) {
          iter.m_MasterUnset.insert(master);
        }
      }
    }
  }

#pragma message("emitting this seems to cause a crash!")
//  emit layoutChanged();
}

QVariant PluginList::data(const QModelIndex &modelIndex, int role) const
{
  int index = modelIndex.row();
  if ((role == Qt::DisplayRole)
      || (role == Qt::EditRole)) {
    switch (modelIndex.column()) {
      case COL_NAME: {
        return m_ESPs[index].m_Name;
      } break;
      case COL_PRIORITY: {
        return m_ESPs[index].m_Priority;
      } break;
      case COL_MODINDEX: {
        return m_ESPs[index].m_Index;
      } break;
      default: {
          return QVariant();
      } break;
    }
  } else if ((role == Qt::CheckStateRole) && (modelIndex.column() == 0)) {
    if (m_ESPs[index].m_ForceEnabled) {
      return QVariant();
    } else {
      return m_ESPs[index].m_Enabled ? Qt::Checked : Qt::Unchecked;
    }
  } else if (role == Qt::ForegroundRole) {
    if ((modelIndex.column() == COL_NAME) &&
      m_ESPs[index].m_ForceEnabled) {
      return QBrush(Qt::gray);
    }
  } else if (role == Qt::BackgroundRole
    || (role == ViewMarkingScrollBar::DEFAULT_ROLE)) {
    if (m_ESPs[index].m_ModSelected) {
      return Settings::instance().colors().pluginListContained();
    } else {
      return QVariant();
    }
  } else if (role == Qt::FontRole) {
    QFont result;
    if (m_ESPs[index].m_IsMaster) {
      result.setItalic(true);
      result.setWeight(QFont::Bold);
    } else if (m_ESPs[index].m_IsLight || m_ESPs[index].m_IsLightFlagged) {
      result.setItalic(true);
    }
    return result;
  } else if (role == Qt::TextAlignmentRole) {
    if (modelIndex.column() == 0) {
      return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    } else {
      return QVariant(Qt::AlignHCenter | Qt::AlignVCenter);
    }
  } else if (role == Qt::ToolTipRole) {
    QString name = m_ESPs[index].m_Name.toLower();
    auto addInfoIter = m_AdditionalInfo.find(name);
    QString toolTip;
    if (addInfoIter != m_AdditionalInfo.end()) {
      if (!addInfoIter->second.m_Messages.isEmpty()) {
        toolTip += addInfoIter->second.m_Messages.join("<br>") + "<br><hr>";
      }
    }
    if (m_ESPs[index].m_ForceEnabled) {
      QString text = tr("<b>Origin</b>: %1").arg(m_ESPs[index].m_OriginName);
      text += tr("<br><b><i>This plugin can't be disabled (enforced by the game).</i></b>");
      toolTip += text;
    } else {
      QString text = tr("<b>Origin</b>: %1").arg(m_ESPs[index].m_OriginName);
      if (m_ESPs[index].m_Author.size() > 0) {
        text += "<br><b>" + tr("Author") + "</b>: " + TruncateString(m_ESPs[index].m_Author);
      }
      if (m_ESPs[index].m_Description.size() > 0) {
        text += "<br><b>" + tr("Description") + "</b>: " + TruncateString(m_ESPs[index].m_Description);
      }
      if (m_ESPs[index].m_MasterUnset.size() > 0) {
        text += "<br><b>" + tr("Missing Masters") + "</b>: <b>" + TruncateString(SetJoin(m_ESPs[index].m_MasterUnset, ", ")) + "</b>";
      }
      std::set<QString> enabledMasters;
      std::set_difference(m_ESPs[index].m_Masters.begin(), m_ESPs[index].m_Masters.end(),
                          m_ESPs[index].m_MasterUnset.begin(), m_ESPs[index].m_MasterUnset.end(),
                          std::inserter(enabledMasters, enabledMasters.end()));
      if (!enabledMasters.empty()) {
        text += "<br><b>" + tr("Enabled Masters") + "</b>: " + TruncateString(SetJoin(enabledMasters, ", "));
      }
      if (!m_ESPs[index].m_Archives.empty()) {
        text += "<br><b>" + tr("Loads Archives") + "</b>: " + TruncateString(SetJoin(m_ESPs[index].m_Archives, ", "));
        text += "<br>" + tr("There are Archives connected to this plugin. "
          "Their assets will be added to your game, overwriting in case of conflicts following the plugin order. "
          "Loose files will always overwrite assets from Archives. (This flag only checks for Archives from the same mod as the plugin)");
      }
      if (m_ESPs[index].m_HasIni) {
        text += "<br><b>" + tr("Loads INI settings") + "</b>: ";
        text += "<br>" + tr("There is an ini file connected to this plugin. "
                            "Its settings will be added to your game settings, overwriting in case of conflicts.");
      }
      if (m_ESPs[index].m_IsLightFlagged && !m_ESPs[index].m_IsLight) {
          text += "<br><br>" + tr("This ESP is flagged as an ESL. "
                                  "It will adhere to the ESP load order but the records will be loaded in ESL space.");
      }
      toolTip += text;
    }
    return toolTip;
  } else if (role == Qt::UserRole + 1) {
    QVariantList result;
    QString nameLower = m_ESPs[index].m_Name.toLower();
    if (m_ESPs[index].m_MasterUnset.size() > 0) {
      result.append(":/MO/gui/warning");
    }
    if (m_LockedOrder.find(nameLower) != m_LockedOrder.end()) {
      result.append(":/MO/gui/locked");
    }
    auto bossInfoIter = m_AdditionalInfo.find(nameLower);
    if (bossInfoIter != m_AdditionalInfo.end()) {
      if (!bossInfoIter->second.m_Messages.isEmpty()) {
        result.append(":/MO/gui/information");
      }
    }
    if (m_ESPs[index].m_HasIni) {
      result.append(":/MO/gui/attachment");
    }
    if (!m_ESPs[index].m_Archives.empty()) {
      result.append(":/MO/gui/archive_conflict_neutral");
    }
    if (m_ESPs[index].m_IsLightFlagged && !m_ESPs[index].m_IsLight) {
        result.append(":/MO/gui/awaiting");
    }
    return result;
  }
  return QVariant();
}


bool PluginList::setData(const QModelIndex &modIndex, const QVariant &value, int role)
{
  QString modName = modIndex.data().toString();
  IPluginList::PluginStates oldState = state(modName);

  bool result = false;

  if (role == Qt::CheckStateRole) {
    m_ESPs[modIndex.row()].m_Enabled =
        value.toInt() == Qt::Checked || m_ESPs[modIndex.row()].m_ForceEnabled;
    m_LastCheck.restart();
    emit dataChanged(modIndex, modIndex);

    refreshLoadOrder();
    emit writePluginsList();

    result = true;
  } else if (role == Qt::EditRole) {
    if (modIndex.column() == COL_PRIORITY) {
      bool ok = false;
      int newPriority = value.toInt(&ok);
      if (ok) {
        setPluginPriority(modIndex.row(), newPriority);
        result = true;
      }
      refreshLoadOrder();
    }
  }

  IPluginList::PluginStates newState = state(modName);
  if (oldState != newState) {
    try {
      m_PluginStateChanged(modName, newState);
      testMasters();
      emit dataChanged(
          this->index(0, 0),
          this->index(static_cast<int>(m_ESPs.size()), columnCount()));
    } catch (const std::exception &e) {
      log::error("failed to invoke state changed notification: {}", e.what());
    } catch (...) {
      log::error("failed to invoke state changed notification: unknown exception");
    }
  }

  return result;
}


QVariant PluginList::headerData(int section, Qt::Orientation orientation,
                             int role) const
{
  if (orientation == Qt::Horizontal) {
    if (role == Qt::DisplayRole) {
      return getColumnName(section);
    } else if (role == Qt::ToolTipRole) {
      return getColumnToolTip(section);
    } else if (role == Qt::SizeHintRole) {
      QSize temp = m_FontMetrics.size(Qt::TextSingleLine, getColumnName(section));
      temp.rwidth() += 25;
      temp.rheight() += 12;
      return temp;
    }
  }
  return QAbstractItemModel::headerData(section, orientation, role);
}


Qt::ItemFlags PluginList::flags(const QModelIndex &modelIndex) const
{
  int index = modelIndex.row();
  Qt::ItemFlags result = QAbstractItemModel::flags(modelIndex);

  if (modelIndex.isValid())  {
    if (!m_ESPs[index].m_ForceEnabled) {
      result |= Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled;
    }
    if (modelIndex.column() == COL_PRIORITY) {
      result |= Qt::ItemIsEditable;
    }
    result &= ~Qt::ItemIsDropEnabled;
  } else {
    result |= Qt::ItemIsDropEnabled;
  }

  return result;
}


void PluginList::setPluginPriority(int row, int &newPriority)
{
  int newPriorityTemp = newPriority;

  // enforce valid range
  if (newPriorityTemp < 0)
    newPriorityTemp = 0;
  else if (newPriorityTemp >= static_cast<int>(m_ESPsByPriority.size()))
    newPriorityTemp = static_cast<int>(m_ESPsByPriority.size()) - 1;

  if (!m_ESPs[row].m_IsMaster && !m_ESPs[row].m_IsLight) {
    // don't allow esps to be moved above esms
    while ((newPriorityTemp < static_cast<int>(m_ESPsByPriority.size() - 1)) &&
            (m_ESPs.at(m_ESPsByPriority.at(newPriorityTemp)).m_IsMaster ||
             m_ESPs.at(m_ESPsByPriority.at(newPriorityTemp)).m_IsLight)) {
      ++newPriorityTemp;
    }
  } else {
    // don't allow esms to be moved below esps
    while ((newPriorityTemp > 0) &&
           !m_ESPs.at(m_ESPsByPriority.at(newPriorityTemp)).m_IsMaster &&
           !m_ESPs.at(m_ESPsByPriority.at(newPriorityTemp)).m_IsLight) {
      --newPriorityTemp;
    }
    // also don't allow "regular" esms to be moved above primary plugins
    while ((newPriorityTemp < static_cast<int>(m_ESPsByPriority.size() - 1)) &&
           (m_ESPs.at(m_ESPsByPriority.at(newPriorityTemp)).m_ForceEnabled)) {
      ++newPriorityTemp;
    }
  }

  try {
    int oldPriority = m_ESPs.at(row).m_Priority;
    if (newPriorityTemp > oldPriority) {
      // priority is higher than the old, so the gap we left is in lower priorities
      for (int i = oldPriority + 1; i <= newPriorityTemp; ++i) {
        --m_ESPs.at(m_ESPsByPriority.at(i)).m_Priority;
      }
      emit dataChanged(index(oldPriority + 1, 0), index(newPriorityTemp, columnCount()));
    } else {
      for (int i = newPriorityTemp; i < oldPriority; ++i) {
        ++m_ESPs.at(m_ESPsByPriority.at(i)).m_Priority;
      }
      emit dataChanged(index(newPriorityTemp, 0), index(oldPriority - 1, columnCount()));
      ++newPriority;
    }

    m_ESPs.at(row).m_Priority = newPriorityTemp;
    emit dataChanged(index(row, 0), index(row, columnCount()));
    m_PluginMoved(m_ESPs[row].m_Name, oldPriority, newPriorityTemp);
  } catch (const std::out_of_range&) {
    reportError(tr("failed to restore load order for %1").arg(m_ESPs[row].m_Name));
  }

  updateIndices();
}


void PluginList::changePluginPriority(std::vector<int> rows, int newPriority)
{
  ChangeBracket<PluginList> layoutChange(this);
  const std::vector<ESPInfo> &esp = m_ESPs;

  int minPriority = INT_MAX;
  int maxPriority = INT_MIN;

  // don't try to move plugins before force-enabled plugins
  for (std::vector<ESPInfo>::const_iterator iter = m_ESPs.begin();
       iter != m_ESPs.end(); ++iter) {
    if (iter->m_ForceEnabled) {
      newPriority = std::max(newPriority, iter->m_Priority+1);
    }
    maxPriority = std::max(maxPriority, iter->m_Priority+1);
    minPriority = std::min(minPriority, iter->m_Priority);
  }

  // limit the new priority to existing priorities
  newPriority = std::min(newPriority, maxPriority);
  newPriority = std::max(newPriority, minPriority);

  // sort the moving plugins by ascending priorities
  std::sort(rows.begin(), rows.end(),
    [&esp](const int &LHS, const int &RHS) {
    return esp[LHS].m_Priority < esp[RHS].m_Priority;
  });

  // if at least on plugin is increasing in priority, the target index is
  // that of the row BELOW the dropped location, otherwise it's the one above
  for (std::vector<int>::const_iterator iter = rows.begin();
    iter != rows.end(); ++iter) {
    if (m_ESPs[*iter].m_Priority < newPriority) {
      --newPriority;
      break;
    }
  }

  for (std::vector<int>::const_iterator iter = rows.begin(); iter != rows.end(); ++iter) {
    setPluginPriority(*iter, newPriority);
  }

  layoutChange.finish();
  refreshLoadOrder();
  emit writePluginsList();
}

bool PluginList::dropMimeData(const QMimeData *mimeData, Qt::DropAction action, int row, int, const QModelIndex &parent)
{
  if (action == Qt::IgnoreAction) {
    return true;
  }

  QByteArray encoded = mimeData->data("application/x-qabstractitemmodeldatalist");
  QDataStream stream(&encoded, QIODevice::ReadOnly);

  std::vector<int> sourceRows;

  while (!stream.atEnd()) {
    int sourceRow, col;
    QMap<int,  QVariant> roleDataMap;
    stream >> sourceRow >> col >> roleDataMap;
    if (col == 0) { // only add each row once
      sourceRows.push_back(sourceRow);
    }
  }

  if (row == -1) {
    row = parent.row();
  }

  int newPriority;

  if ((row < 0) ||
      (row >= static_cast<int>(m_ESPs.size()))) {
    newPriority = static_cast<int>(m_ESPs.size());
  } else {
    newPriority = m_ESPs[row].m_Priority;
  }
  changePluginPriority(sourceRows, newPriority);

  return false;
}

QModelIndex PluginList::index(int row, int column, const QModelIndex&) const
{
  if ((row < 0) || (row >= rowCount()) || (column < 0) || (column >= columnCount())) {
    return QModelIndex();
  }
  return createIndex(row, column, row);
}

QModelIndex PluginList::parent(const QModelIndex&) const
{
  return QModelIndex();
}


bool PluginList::eventFilter(QObject *obj, QEvent *event)
{
  if (event->type() == QEvent::KeyPress) {
    QAbstractItemView *itemView = qobject_cast<QAbstractItemView*>(obj);

    if (itemView == nullptr) {
      return QAbstractItemModel::eventFilter(obj, event);
    }

    QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
    // ctrl+up and ctrl+down -> increase or decrease priority of selected plugins
    if ((keyEvent->modifiers() == Qt::ControlModifier) &&
        ((keyEvent->key() == Qt::Key_Up) || (keyEvent->key() == Qt::Key_Down))) {
      QItemSelectionModel *selectionModel = itemView->selectionModel();
      const QSortFilterProxyModel *proxyModel = qobject_cast<const QSortFilterProxyModel*>(selectionModel->model());
      if (proxyModel != nullptr) {
        int diff = -1;
        if (((keyEvent->key() == Qt::Key_Up) && (proxyModel->sortOrder() == Qt::DescendingOrder)) ||
            ((keyEvent->key() == Qt::Key_Down) && (proxyModel->sortOrder() == Qt::AscendingOrder))) {
          diff = 1;
        }
        QModelIndexList rows = selectionModel->selectedRows();
        // remove elements that aren't supposed to be movable
        QMutableListIterator<QModelIndex> iter(rows);
        while (iter.hasNext()) {
          if ((iter.next().flags() & Qt::ItemIsDragEnabled) == 0) {
            iter.remove();
          }
        }
        if (keyEvent->key() == Qt::Key_Down) {
          for (int i = 0; i < rows.size() / 2; ++i) {
            rows.swapItemsAt(i, rows.size() - i - 1);
          }
        }
        for (QModelIndex idx : rows) {
          idx = proxyModel->mapToSource(idx);
          int newPriority = m_ESPs[idx.row()].m_Priority + diff;
          if ((newPriority >= 0) && (newPriority < rowCount())) {
            setPluginPriority(idx.row(), newPriority);
          }
        }
        refreshLoadOrder();
      }
      return true;
    } else if (keyEvent->key() == Qt::Key_Space) {
      QItemSelectionModel *selectionModel = itemView->selectionModel();
      const QSortFilterProxyModel *proxyModel = qobject_cast<const QSortFilterProxyModel*>(selectionModel->model());
      QList<QPersistentModelIndex> indices;
      for (QModelIndex idx : selectionModel->selectedRows()) {
        indices.append(idx);
      }

      QModelIndex minRow, maxRow;
      for (QModelIndex idx : indices) {
        if (proxyModel != nullptr) {
          idx = proxyModel->mapToSource(idx);
        }
        if (!minRow.isValid() || (idx.row() < minRow.row())) {
          minRow = idx;
        }
        if (!maxRow.isValid() || (idx.row() > maxRow.row())) {
          maxRow = idx;
        }
        int oldState = idx.data(Qt::CheckStateRole).toInt();
        setData(idx, oldState == Qt::Unchecked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
      }
      emit dataChanged(minRow, maxRow);

      return true;
    }
  }
  return QAbstractItemModel::eventFilter(obj, event);
}


PluginList::ESPInfo::ESPInfo(const QString &name, bool enabled,
                             const QString &originName, const QString &fullPath,
                             bool hasIni, std::set<QString> archives, bool lightPluginsAreSupported)
  : m_Name(name), m_FullPath(fullPath), m_Enabled(enabled), m_ForceEnabled(enabled),
    m_Priority(0), m_LoadOrder(-1), m_OriginName(originName), m_HasIni(hasIni), m_Archives(archives), m_ModSelected(false)
{
  try {
    ESP::File file(ToWString(fullPath));
    m_IsMaster = file.isMaster();
    auto extension = name.right(3).toLower();
    m_IsLight = lightPluginsAreSupported && (extension == "esl");
    m_IsLightFlagged = lightPluginsAreSupported && file.isLight();

    m_Author = QString::fromLatin1(file.author().c_str());
    m_Description = QString::fromLatin1(file.description().c_str());
    std::set<std::string> masters = file.masters();
    for (auto iter = masters.begin(); iter != masters.end(); ++iter) {
      m_Masters.insert(QString(iter->c_str()));
    }
  } catch (const std::exception &e) {
    log::error("failed to parse plugin file {}: {}", fullPath, e.what());
    m_IsMaster = false;
    m_IsLight = false;
    m_IsLightFlagged = false;
  }
}

void PluginList::managedGameChanged(const IPluginGame *gamePlugin)
{
  m_GamePlugin = gamePlugin;
}
