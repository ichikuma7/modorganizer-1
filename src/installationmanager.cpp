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

#include "installationmanager.h"

#include "utility.h"
#include "report.h"
#include "categories.h"
#include "questionboxmemory.h"
#include "settings.h"
#include "queryoverwritedialog.h"
#include "messagedialog.h"
#include "iplugininstallersimple.h"
#include "iplugininstallercustom.h"
#include "nexusinterface.h"
#include "selectiondialog.h"
#include "modinfo.h"
#include <scopeguard.h>
#include <installationtester.h>
#include <utility.h>
#include <scopeguard.h>

#include <QFileInfo>
#include <QLibrary>
#include <QInputDialog>
#include <QRegExp>
#include <QDir>
#include <QMessageBox>
#include <QSettings>
#include <QPushButton>
#include <QApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QDebug>
#include <QTextDocument>
#include <QtConcurrent/QtConcurrentRun>

#include <Shellapi.h>

#include <boost/assign.hpp>
#include <boost/scoped_ptr.hpp>


using namespace MOBase;
using namespace MOShared;


typedef Archive* (*CreateArchiveType)();



template <typename T>
static T resolveFunction(QLibrary &lib, const char *name)
{
  T temp = reinterpret_cast<T>(lib.resolve(name));
  if (temp == nullptr) {
    throw std::runtime_error(QObject::tr("invalid 7-zip32.dll: %1")
                                 .arg(lib.errorString())
                                 .toLatin1()
                                 .constData());
  }
  return temp;
}

InstallationManager::InstallationManager()
    : m_ParentWidget(nullptr),
      m_SupportedExtensions({"zip", "rar", "7z", "fomod", "001"}),
      m_IsRunning(false) {
  QLibrary archiveLib(QCoreApplication::applicationDirPath() +
                      "\\dlls\\archive.dll");
  if (!archiveLib.load()) {
    throw MyException(QObject::tr("archive.dll not loaded: \"%1\"")
                          .arg(archiveLib.errorString()));
  }

  CreateArchiveType CreateArchiveFunc
      = resolveFunction<CreateArchiveType>(archiveLib, "CreateArchive");

  m_ArchiveHandler = CreateArchiveFunc();
  if (!m_ArchiveHandler->isValid()) {
    throw MyException(getErrorString(m_ArchiveHandler->getLastError()));
  }
}

InstallationManager::~InstallationManager()
{
  delete m_ArchiveHandler;
}

void InstallationManager::setParentWidget(QWidget *widget)
{
  for (IPluginInstaller *installer : m_Installers) {
    installer->setParentWidget(widget);
  }
}

void InstallationManager::setURL(QString const &url)
{
  m_URL = url;
}

void InstallationManager::queryPassword(QString *password)
{
  *password = QInputDialog::getText(nullptr, tr("Password required"),
                                    tr("Password"), QLineEdit::Password);
}

void InstallationManager::mapToArchive(const DirectoryTree::Node *node, QString path, FileData * const *data)
{
  if (path.length() > 0) {
    // when using a long windows path (starting with \\?\) we apparently can have redundant
    // . components in the path. This wasn't a problem with "regular" path names.
    if (path == ".") {
      path.clear();
    } else {
      path.append("\\");
    }
  }

  for (DirectoryTree::const_leaf_iterator iter = node->leafsBegin(); iter != node->leafsEnd(); ++iter) {
    data[iter->getIndex()]->addOutputFileName(path + iter->getName().toQString());
  }

  for (DirectoryTree::const_node_iterator iter = node->nodesBegin(); iter != node->nodesEnd(); ++iter) {
    QString temp = path + (*iter)->getData().name.toQString();
    if ((*iter)->getData().index != -1) {
      data[(*iter)->getData().index]->addOutputFileName(temp);
    }
    mapToArchive(*iter, temp, data);
  }
}


void InstallationManager::mapToArchive(const DirectoryTree::Node *baseNode)
{
  FileData* const *data;
  size_t size;
  m_ArchiveHandler->getFileList(data, size);

  mapToArchive(baseNode, "", data);
}


bool InstallationManager::unpackSingleFile(const QString &fileName)
{
  FileData* const *data;
  size_t size;
  m_ArchiveHandler->getFileList(data, size);

  QString baseName = QFileInfo(fileName).fileName();

  bool available = false;
  for (size_t i = 0; i < size; ++i) {
    if (data[i]->getFileName().compare(fileName, Qt::CaseInsensitive) == 0) {
      available = true;
      data[i]->addOutputFileName(baseName);
      m_TempFilesToDelete.insert(baseName);
    }
  }

  if (!available) {
    return false;
  }

  m_InstallationProgress = new QProgressDialog(m_ParentWidget);
  ON_BLOCK_EXIT([this] () {
    m_InstallationProgress->hide();
    m_InstallationProgress->deleteLater();
    m_InstallationProgress = nullptr;
    m_Progress = 0;
  });
  m_InstallationProgress->setWindowFlags(
        m_InstallationProgress->windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_InstallationProgress->setWindowTitle(tr("Extracting files"));
  m_InstallationProgress->setWindowModality(Qt::WindowModal);
  m_InstallationProgress->setFixedSize(600, 100);
  m_InstallationProgress->show();

  QFuture<bool> future = QtConcurrent::run([&]() -> bool {
    return m_ArchiveHandler->extract(
      QDir::tempPath(),
      new MethodCallback<InstallationManager, void, float>(this, &InstallationManager::updateProgress),
      nullptr,
      new MethodCallback<InstallationManager, void, QString const &>(this, &InstallationManager::report7ZipError)
    );
  });
  do {
    if (m_Progress != m_InstallationProgress->value())
      m_InstallationProgress->setValue(m_Progress);
    QCoreApplication::processEvents();
  } while (!future.isFinished() || m_InstallationProgress->isVisible());
  bool res = future.result();

  return res;
}


QString InstallationManager::extractFile(const QString &fileName)
{
  if (unpackSingleFile(fileName)) {
    return QDir::tempPath() + "/" + QFileInfo(fileName).fileName();
  } else {
    return QString();
  }
}


static QString canonicalize(const QString &name)
{
  QString result(name);
  if ((result.startsWith('/')) ||
      (result.startsWith('\\'))) {
    result.remove(0, 1);
  }
  result.replace('/', '\\');

  return result;
}


QStringList InstallationManager::extractFiles(const QStringList &filesOrig, bool flatten)
{
  QStringList files;

  for (const QString &file : filesOrig) {
    files.append(canonicalize(file));
  }

  QStringList result;

  FileData* const *data;
  size_t size;
  m_ArchiveHandler->getFileList(data, size);

  for (size_t i = 0; i < size; ++i) {
    //FIXME Use qstring all the way through
    if (files.contains(data[i]->getFileName(), Qt::CaseInsensitive)) {
      std::wstring temp = data[i]->getFileName().toStdWString();
      wchar_t const * const origFile = temp.c_str();
      const wchar_t *targetFile = origFile;
      //Note: I don't think 'flatten' is ever set to true. so this code
      //might never be executed
      if (flatten) {
        targetFile = wcsrchr(origFile/*data[i]->getFileName()*/, '\\');
        if (targetFile == nullptr) {
          targetFile = wcsrchr(origFile/*data[i]->getFileName()*/, '/');
        }
        if (targetFile == nullptr) {
          log::error("Failed to find backslash in {}", data[i]->getFileName());
          continue;
        } else {
          // skip the slash
          ++targetFile;
        }
      }
      data[i]->addOutputFileName(ToQString(targetFile));

      result.append(QDir::tempPath().append("/").append(ToQString(targetFile)));

      m_TempFilesToDelete.insert(ToQString(targetFile));
    }
  }

  m_InstallationProgress = new QProgressDialog(m_ParentWidget);
  ON_BLOCK_EXIT([this] () {
    m_InstallationProgress->hide();
    m_InstallationProgress->deleteLater();
    m_InstallationProgress = nullptr;
    m_Progress = 0;
  });
  m_InstallationProgress->setWindowFlags(
        m_InstallationProgress->windowFlags() & (~Qt::WindowContextHelpButtonHint));
  m_InstallationProgress->setWindowTitle(tr("Extracting files"));
  m_InstallationProgress->setWindowModality(Qt::WindowModal);
  m_InstallationProgress->setFixedSize(600, 100);
  m_InstallationProgress->show();

  // unpack only the files we need for the installer
  QFuture<bool> future = QtConcurrent::run([&]() -> bool {
    return m_ArchiveHandler->extract(
      QDir::tempPath(),
      new MethodCallback<InstallationManager, void, float>(this, &InstallationManager::updateProgress),
      nullptr,
      new MethodCallback<InstallationManager, void, QString const &>(this, &InstallationManager::report7ZipError)
    );
  });
  do {
    if (m_Progress != m_InstallationProgress->value())
      m_InstallationProgress->setValue(m_Progress);
    QCoreApplication::processEvents();
  } while (!future.isFinished() || m_InstallationProgress->isVisible());
  if (!future.result()) {
    throw MyException(tr("Extraction failed: %1").arg(m_ArchiveHandler->getLastError()));
  }

  return result;
}

IPluginInstaller::EInstallResult InstallationManager::installArchive(GuessedValue<QString> &modName, const QString &archiveName, const int &modId)
{
  // in earlier versions the modName was copied here and the copy passed to install. I don't know why I did this and it causes
  // a problem if this is called by the bundle installer and the bundled installer adds additional names that then end up being used,
  // because the caller will then not have the right name.
  bool iniTweaks;
  if (install(archiveName, modName, iniTweaks, modId)) {
    return IPluginInstaller::RESULT_SUCCESS;
  } else {
    return IPluginInstaller::RESULT_FAILED;
  }
}


DirectoryTree *InstallationManager::createFilesTree()
{
  FileData* const *data;
  size_t size;
  m_ArchiveHandler->getFileList(data, size);

  QScopedPointer<DirectoryTree> result(new DirectoryTree);

  for (size_t i = 0; i < size; ++i) {
    // the files are in a flat list where each file has a a full path relative to the archive root
    // to create a tree, we have to iterate over each path component of each. This could be sped up by
    // grouping the filenames first, but so far there doesn't seem to be an actual performance problem
    DirectoryTree::Node *currentNode = result.data();

    QString fileName = data[i]->getFileName();
    QStringList components = fileName.split("\\");

    // iterate over all path-components of this filename (including the filename itself)
    for (QStringList::iterator componentIter = components.begin(); componentIter != components.end(); ++componentIter) {
      if (componentIter->size() == 0) {
        // empty string indicates fileName is actually only a directory name and we have
        // completely processed it already.
        break;
      }

      bool exists = false;
      // test if this path is already in the tree
      for (DirectoryTree::node_iterator nodeIter = currentNode->nodesBegin(); nodeIter != currentNode->nodesEnd(); ++nodeIter) {
        if ((*nodeIter)->getData().name == *componentIter) {
          currentNode = *nodeIter;
          exists = true;
          break;
        }
      }

      if (!exists) {
        if (componentIter + 1 == components.end()) {
          // last path component. directory or file?
          if (data[i]->isDirectory()) {
            // this is a bit problematic. archives will often only list directories if they are empty,
            // otherwise the dir only appears in the path of a file. In the UI however we allow the user
            // to uncheck all files in a directory while keeping the dir checked. Those directories are
            // currently not installed.
            DirectoryTree::Node *newNode = new DirectoryTree::Node;
            newNode->setData(
                DirectoryTreeInformation(*componentIter, static_cast<int>(i)));
            currentNode->addNode(newNode, false);
            currentNode = newNode;
          } else {
            currentNode->addLeaf(FileTreeInformation(*componentIter, i));
          }
        } else {
          DirectoryTree::Node *newNode = new DirectoryTree::Node;
          newNode->setData(DirectoryTreeInformation(*componentIter, -1));
          currentNode->addNode(newNode, false);
          currentNode = newNode;
        }
      }
    }
  }

  return result.take();
}


bool InstallationManager::isSimpleArchiveTopLayer(const DirectoryTree::Node *node, bool bainStyle)
{
  // see if there is at least one directory that makes sense on the top level
  for (DirectoryTree::const_node_iterator iter = node->nodesBegin(); iter != node->nodesEnd(); ++iter) {
    if ((bainStyle && InstallationTester::isTopLevelDirectoryBain((*iter)->getData().name)) ||
        (!bainStyle && InstallationTester::isTopLevelDirectory((*iter)->getData().name))) {
      log::debug("{} on the top level", (*iter)->getData().name.toQString());
      return true;
    }
  }

  // see if there is a file that makes sense on the top level
  for (DirectoryTree::const_leaf_iterator iter = node->leafsBegin(); iter != node->leafsEnd(); ++iter) {
    if (InstallationTester::isTopLevelSuffix(iter->getName())) {
      return true;
    }
  }
  return false;
}


DirectoryTree::Node *InstallationManager::getSimpleArchiveBase(DirectoryTree *dataTree)
{
  DirectoryTree::Node *currentNode = dataTree;

  while (true) {
    if (isSimpleArchiveTopLayer(currentNode, false)) {
      return currentNode;
    } else if ((currentNode->numLeafs() == 0) &&
               (currentNode->numNodes() == 1)) {
      currentNode = *currentNode->nodesBegin();
    } else {
      log::debug("not a simple archive");
      return nullptr;
    }
  }
}


void InstallationManager::updateProgress(float percentage)
{
  if (m_InstallationProgress != nullptr) {
    m_Progress = static_cast<int>(percentage * 100.0);

    if (m_InstallationProgress->wasCanceled()) {
      m_ArchiveHandler->cancel();
      m_InstallationProgress->reset();
    }
  }
}


void InstallationManager::updateProgressFile(QString const &fileName)
{
  m_ProgressFile = fileName;
}


void InstallationManager::report7ZipError(QString const &errorMessage)
{
  m_ErrorMessage = errorMessage;
  m_ArchiveHandler->cancel();
}


QString InstallationManager::generateBackupName(const QString &directoryName) const
{
  QString backupName = directoryName + "_backup";
  if (QDir(backupName).exists()) {
    int idx = 2;
    QString temp = backupName + QString::number(idx);
    while (QDir(temp).exists()) {
      ++idx;
      temp = backupName + QString::number(idx);
    }
    backupName = temp;
  }
  return backupName;
}


bool InstallationManager::testOverwrite(GuessedValue<QString> &modName, bool *merge) const
{
  QString targetDirectory = QDir::fromNativeSeparators(m_ModsDirectory + "\\" + modName);

  while (QDir(targetDirectory).exists()) {
    Settings &settings(Settings::instance());

    const bool backup = settings.keepBackupOnInstall();
    QueryOverwriteDialog overwriteDialog(
      m_ParentWidget,
      backup ? QueryOverwriteDialog::BACKUP_YES : QueryOverwriteDialog::BACKUP_NO);

    if (overwriteDialog.exec()) {
      settings.setKeepBackupOnInstall(overwriteDialog.backup());

      if (overwriteDialog.backup()) {
        QString backupDirectory = generateBackupName(targetDirectory);
        if (!copyDir(targetDirectory, backupDirectory, false)) {
          reportError(tr("Failed to create backup"));
          return false;
        }
      }
      if (merge != nullptr) {
        *merge = (overwriteDialog.action() == QueryOverwriteDialog::ACT_MERGE);
      }
      if (overwriteDialog.action() == QueryOverwriteDialog::ACT_RENAME) {
        bool ok = false;
        QString name = QInputDialog::getText(m_ParentWidget, tr("Mod Name"), tr("Name"),
                                             QLineEdit::Normal, modName, &ok);
        if (ok && !name.isEmpty()) {
          modName.update(name, GUESS_USER);
          if (!ensureValidModName(modName)) {
            return false;
          }
          targetDirectory = QDir::fromNativeSeparators(m_ModsDirectory) + "/" + modName;
        }
      } else if (overwriteDialog.action() == QueryOverwriteDialog::ACT_REPLACE) {
        // save original settings like categories. Because it makes sense
        QString metaFilename = targetDirectory + "/meta.ini";
        QFile settingsFile(metaFilename);
        QByteArray originalSettings;
        if (settingsFile.open(QIODevice::ReadOnly)) {
          originalSettings = settingsFile.readAll();
          settingsFile.close();
        }

        // remove the directory with all content, then recreate it empty
        shellDelete(QStringList(targetDirectory));
        if (!QDir().mkdir(targetDirectory)) {
          // windows may keep the directory around for a moment, preventing its re-creation. Not sure
          // if this still happens with shellDelete
          Sleep(100);
          QDir().mkdir(targetDirectory);
        }
        // restore the saved settings
        if (settingsFile.open(QIODevice::WriteOnly)) {
          settingsFile.write(originalSettings);
          settingsFile.close();
        } else {
          log::error("failed to restore original settings: {}", metaFilename);
        }
        return true;
      } else if (overwriteDialog.action() == QueryOverwriteDialog::ACT_MERGE) {
        return true;
      }
    } else {
      return false;
    }
  }

  QDir().mkdir(targetDirectory);

  return true;
}


bool InstallationManager::ensureValidModName(GuessedValue<QString> &name) const
{
  while (name->isEmpty()) {
    bool ok;
    name.update(QInputDialog::getText(m_ParentWidget, tr("Invalid name"),
                                      tr("The name you entered is invalid, please enter a different one."),
                                      QLineEdit::Normal, "", &ok),
                GUESS_USER);
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool InstallationManager::doInstall(GuessedValue<QString> &modName, QString gameName, int modID,
                                    const QString &version, const QString &newestVersion,
                                    int categoryID, int fileCategoryID, const QString &repository)
{
  if (!ensureValidModName(modName)) {
    return false;
  }

  bool merge = false;
  // determine target directory
  if (!testOverwrite(modName, &merge)) {
    return false;
  }

  QString targetDirectory = QDir(m_ModsDirectory + "/" + modName).canonicalPath();
  QString targetDirectoryNative = QDir::toNativeSeparators(targetDirectory);

  log::debug("installing to \"{}\"", targetDirectoryNative);

  m_InstallationProgress = new QProgressDialog(m_ParentWidget);
  ON_BLOCK_EXIT([this] () {
    m_InstallationProgress->cancel();
    m_InstallationProgress->hide();
    m_InstallationProgress->deleteLater();
    m_InstallationProgress = nullptr;
    m_Progress = 0;
    m_ProgressFile = QString();
  });

  m_InstallationProgress->setWindowFlags(
        m_InstallationProgress->windowFlags() & (~Qt::WindowContextHelpButtonHint));
  m_InstallationProgress->setWindowModality(Qt::WindowModal);
  m_InstallationProgress->setFixedSize(600, 100);
  m_InstallationProgress->show();
  QFuture<bool> future = QtConcurrent::run([&]() -> bool {
    return m_ArchiveHandler->extract(
      targetDirectory,
      new MethodCallback<InstallationManager, void, float>(this, &InstallationManager::updateProgress),
      new MethodCallback<InstallationManager, void, QString const &>(this, &InstallationManager::updateProgressFile),
      new MethodCallback<InstallationManager, void, QString const &>(this, &InstallationManager::report7ZipError)
    );
  });
  do {
    if (m_Progress != m_InstallationProgress->value())
      m_InstallationProgress->setValue(m_Progress);
    if (m_ProgressFile != m_InstallationProgress->labelText())
      m_InstallationProgress->setLabelText(m_ProgressFile);
    QCoreApplication::processEvents();
  } while (!future.isFinished());
  if (!future.result()) {
    if (m_ArchiveHandler->getLastError() == Archive::ERROR_EXTRACT_CANCELLED) {
      if (!m_ErrorMessage.isEmpty()) {
        throw MyException(tr("Extraction failed: %1").arg(m_ErrorMessage));
      } else {
      return false;
      }
    } else {
      throw MyException(tr("Extraction failed: %1").arg(m_ArchiveHandler->getLastError()));
    }
  }

  QSettings settingsFile(targetDirectory + "/meta.ini", QSettings::IniFormat);

  // overwrite settings only if they are actually are available or haven't been set before
  if ((gameName != "") || !settingsFile.contains("gameName")) {
    settingsFile.setValue("gameName", gameName);
  }
  if ((modID != 0) || !settingsFile.contains("modid")) {
    settingsFile.setValue("modid", modID);
  }
  if (!settingsFile.contains("version") ||
      (!version.isEmpty() &&
       (!merge || (VersionInfo(version) >= VersionInfo(settingsFile.value("version").toString()))))) {
    settingsFile.setValue("version", version);
  }
  if (!newestVersion.isEmpty() || !settingsFile.contains("newestVersion")) {
    settingsFile.setValue("newestVersion", newestVersion);
  }
  // issue #51 used to overwrite the manually set categories
  if (!settingsFile.contains("category")) {
    settingsFile.setValue("category", QString::number(categoryID));
  }
  settingsFile.setValue("nexusFileStatus", fileCategoryID);
  settingsFile.setValue("installationFile", m_CurrentFile);
  settingsFile.setValue("repository", repository);
  settingsFile.setValue("url", m_URL);

  //cleanup of m_URL or this will persist across installs.
  m_URL = "";

  if (!merge) {
    // this does not clear the list we have in memory but the mod is going to have to be re-read anyway
    // btw.: installedFiles were written with beginWriteArray but we can still clear it with beginGroup. nice
    settingsFile.beginGroup("installedFiles");
    settingsFile.remove("");
    settingsFile.endGroup();
  }

  return true;
}


bool InstallationManager::wasCancelled()
{
  return m_ArchiveHandler->getLastError() == Archive::ERROR_EXTRACT_CANCELLED;
}

bool InstallationManager::isRunning()
{
  return m_IsRunning;
}


void InstallationManager::postInstallCleanup()
{
  m_ArchiveHandler->close();

  // directories we may want to remove. sorted from longest to shortest to ensure we remove subdirectories first.
  auto longestFirst = [](const QString &LHS, const QString &RHS) -> bool {
                          if (LHS.size() != RHS.size()) return LHS.size() > RHS.size();
                          else return LHS < RHS;
                        };

  std::set<QString, std::function<bool(const QString&, const QString&)>> directoriesToRemove(longestFirst);

  // clean up temp files
  // TODO: this doesn't yet remove directories. Also, the files may be left there if this point isn't reached
  for (const QString &tempFile : m_TempFilesToDelete) {
    QFileInfo fileInfo(QDir::tempPath() + "/" + tempFile);
    if (fileInfo.exists()) {
      if (!fileInfo.isReadable() || !fileInfo.isWritable()) {
        QFile::setPermissions(fileInfo.absoluteFilePath(), QFile::ReadOther | QFile::WriteOther);
      }
      if (!QFile::remove(fileInfo.absoluteFilePath())) {
        log::warn("Unable to delete {}", fileInfo.absoluteFilePath());
      }
    }
    directoriesToRemove.insert(fileInfo.absolutePath());
  }

  m_TempFilesToDelete.clear();

  // try to delete each directory we had temporary files in. the call fails for non-empty directories which is ok
  for (const QString &dir : directoriesToRemove) {
    QDir().rmdir(dir);
  }
}

bool InstallationManager::install(const QString &fileName,
                                  GuessedValue<QString> &modName,
                                  bool &hasIniTweaks,
                                  int modID)
{
  m_IsRunning = true;
  ON_BLOCK_EXIT([this]() { m_IsRunning = false; });

  QFileInfo fileInfo(fileName);
  if (m_SupportedExtensions.find(fileInfo.suffix()) == m_SupportedExtensions.end()) {
    reportError(tr("File format \"%1\" not supported").arg(fileInfo.suffix()));
    return false;
  }

  modName.setFilter(&fixDirectoryName);

  modName.update(QFileInfo(fileName).completeBaseName(), GUESS_FALLBACK);

  // read out meta information from the download if available
  QString gameName = "";
  QString version = "";
  QString newestVersion = "";
  int categoryID = 0;
  int fileCategoryID = 1;
  QString repository = "Nexus";

  QString metaName = fileName + ".meta";
  if (QFile(metaName).exists()) {
    QSettings metaFile(metaName, QSettings::IniFormat);
    gameName = metaFile.value("gameName", "").toString();
    modID = metaFile.value("modID", 0).toInt();
    QTextDocument doc;
    doc.setHtml(metaFile.value("name", "").toString());
    modName.update(doc.toPlainText(), GUESS_FALLBACK);
    modName.update(metaFile.value("modName", "").toString(), GUESS_META);

    version = metaFile.value("version", "").toString();
    newestVersion = metaFile.value("newestVersion", "").toString();
    unsigned int categoryIndex = CategoryFactory::instance().resolveNexusID(
        metaFile.value("category", 0).toInt());
    categoryID = CategoryFactory::instance().getCategoryID(categoryIndex);
    repository = metaFile.value("repository", "").toString();
    fileCategoryID = metaFile.value("fileCategory", 1).toInt();
  }

  if (version.isEmpty()) {
    QDateTime lastMod = fileInfo.lastModified();
    version = "d" + lastMod.toString("yyyy.M.d");
  }

  { // guess the mod name and mod if from the file name if there was no meta information
    QString guessedModName;
    int guessedModID = modID;
    NexusInterface::interpretNexusFileName(QFileInfo(fileName).fileName(), guessedModName, guessedModID, false);
    if ((modID == 0) && (guessedModID != -1)) {
      modID = guessedModID;
    } else if (modID != guessedModID) {
      log::debug("passed mod id: {}, guessed id: {}", modID, guessedModID);
    }

    modName.update(guessedModName, GUESS_GOOD);
  }

  m_CurrentFile = fileInfo.absoluteFilePath();
  if (fileInfo.dir() == QDir(m_DownloadsDirectory)) {
    m_CurrentFile = fileInfo.fileName();
  }
  log::debug("using mod name \"{}\" (id {}) -> {}", QString(modName), modID, m_CurrentFile);

  //If there's an archive already open, close it. This happens with the bundle
  //installer when it uncompresses a split archive, then finds it has a real archive
  //to deal with.
  m_ArchiveHandler->close();

  // open the archive and construct the directory tree the installers work on
  bool archiveOpen = m_ArchiveHandler->open(fileName,
                                            new MethodCallback<InstallationManager, void, QString *>(this, &InstallationManager::queryPassword));
  if (!archiveOpen) {
    log::debug("integrated archiver can't open {}: {} ({})",
           fileName,
           getErrorString(m_ArchiveHandler->getLastError()),
           m_ArchiveHandler->getLastError());
  }
  ON_BLOCK_EXIT(std::bind(&InstallationManager::postInstallCleanup, this));

  QScopedPointer<DirectoryTree> filesTree(archiveOpen ? createFilesTree() : nullptr);
  IPluginInstaller::EInstallResult installResult = IPluginInstaller::RESULT_NOTATTEMPTED;

  std::sort(m_Installers.begin(), m_Installers.end(), [] (IPluginInstaller *LHS, IPluginInstaller *RHS) {
            return LHS->priority() > RHS->priority();
      });

  for (IPluginInstaller *installer : m_Installers) {
    // don't use inactive installers (installer can't be null here but vc static code analysis thinks it could)
    if ((installer == nullptr) || !installer->isActive()) {
      continue;
    }

    // try only manual installers if that was requested
    if (installResult == IPluginInstaller::RESULT_MANUALREQUESTED) {
      if (!installer->isManualInstaller()) {
        continue;
      }
    } else if (installResult != IPluginInstaller::RESULT_NOTATTEMPTED) {
      break;
    }

    try {
      { // simple case
        IPluginInstallerSimple *installerSimple
            = dynamic_cast<IPluginInstallerSimple *>(installer);
        if ((installerSimple != nullptr) && (filesTree != nullptr)
            && (installer->isArchiveSupported(*filesTree))) {
          installResult
              = installerSimple->install(modName, *filesTree, version, modID);
          if (installResult == IPluginInstaller::RESULT_SUCCESS) {
            mapToArchive(filesTree.data());
            // the simple installer only prepares the installation, the rest
            // works the same for all installers
            if (!doInstall(modName, gameName, modID, version, newestVersion, categoryID,
                            fileCategoryID, repository)) {
              installResult = IPluginInstaller::RESULT_FAILED;
            }
          }
        }
      }

      { // custom case
        IPluginInstallerCustom *installerCustom
            = dynamic_cast<IPluginInstallerCustom *>(installer);
        if ((installerCustom != nullptr)
            && (((filesTree != nullptr)
                 && installer->isArchiveSupported(*filesTree))
                || ((filesTree == nullptr)
                    && installerCustom->isArchiveSupported(fileName)))) {
          std::set<QString> installerExt
              = installerCustom->supportedExtensions();
          if (installerExt.find(fileInfo.suffix()) != installerExt.end()) {
            installResult
                = installerCustom->install(modName, gameName, fileName, version, modID);
            unsigned int idx = ModInfo::getIndex(modName);
            if (idx != UINT_MAX) {
              ModInfo::Ptr info = ModInfo::getByIndex(idx);
              info->setRepository(repository);
            }
          }
        }
      }
    } catch (const IncompatibilityException &e) {
      log::error("plugin \"{}\" incompatible: {}", installer->name(), e.what());
    }

    // act upon the installation result. at this point the files have already been
    // extracted to the correct location
    switch (installResult) {
      case IPluginInstaller::RESULT_CANCELED:
      case IPluginInstaller::RESULT_FAILED: {
        return false;
      } break;
      case IPluginInstaller::RESULT_SUCCESS:
      case IPluginInstaller::RESULT_SUCCESSCANCEL: {
        if (filesTree != nullptr) {
          DirectoryTree::node_iterator iniTweakNode = filesTree->nodeFind(DirectoryTreeInformation("INI Tweaks"));
          hasIniTweaks = (iniTweakNode != filesTree->nodesEnd()) &&
                         ((*iniTweakNode)->numLeafs() != 0);
          return true;
        } else {
          return false;
        }
      } break;
    }
  }

  reportError(tr("None of the available installer plugins were able to handle that archive.\n"
    "This is likely due to a corrupted or incompatible download or unrecognized archive format."));

  return false;
}



QString InstallationManager::getErrorString(Archive::Error errorCode)
{
  switch (errorCode) {
    case Archive::ERROR_NONE: {
      return tr("no error");
    } break;
    case Archive::ERROR_LIBRARY_NOT_FOUND: {
      return tr("7z.dll not found");
    } break;
    case Archive::ERROR_LIBRARY_INVALID: {
      return tr("7z.dll isn't valid");
    } break;
    case Archive::ERROR_ARCHIVE_NOT_FOUND: {
      return tr("archive not found");
    } break;
    case Archive::ERROR_FAILED_TO_OPEN_ARCHIVE: {
      return tr("failed to open archive");
    } break;
    case Archive::ERROR_INVALID_ARCHIVE_FORMAT: {
      return tr("unsupported archive type");
    } break;
    case Archive::ERROR_LIBRARY_ERROR: {
      return tr("internal library error");
    } break;
    case Archive::ERROR_ARCHIVE_INVALID: {
      return tr("archive invalid");
    } break;
    default: {
      // this probably means the archiver.dll is newer than this
      return tr("unknown archive error");
    } break;
  }
}


void InstallationManager::registerInstaller(IPluginInstaller *installer)
{
  m_Installers.push_back(installer);
  installer->setInstallationManager(this);
  IPluginInstallerCustom *installerCustom = dynamic_cast<IPluginInstallerCustom*>(installer);
  if (installerCustom != nullptr) {
    std::set<QString> extensions = installerCustom->supportedExtensions();
    m_SupportedExtensions.insert(extensions.begin(), extensions.end());
  }
}

QStringList InstallationManager::getSupportedExtensions() const
{
  QStringList result;
  foreach (const QString &extension, m_SupportedExtensions) {
    result.append(extension);
  }
  return result;
}
