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

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "bsafolder.h"
#include "browserdialog.h"
#include "delayedfilewriter.h"
#include "errorcodes.h"
#include "imoinfo.h"
#include "iuserinterface.h"
#include "modinfo.h"
#include "modlistsortproxy.h"
#include "savegameinfo.h"
#include "tutorialcontrol.h"
#include "plugincontainer.h" //class PluginContainer;
#include "iplugingame.h" //namespace MOBase { class IPluginGame; }
#include <log.h>

//Note the commented headers here can be replaced with forward references,
//when I get round to cleaning up main.cpp
class Executable;
class CategoryFactory;
class OrganizerCore;

class PluginListSortProxy;
namespace BSA { class Archive; }

namespace MOBase { class IPluginModPage; }
namespace MOBase { class IPluginTool; }
namespace MOBase { class ISaveGame; }

namespace MOShared { class DirectoryEntry; }

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QList>
#include <QMainWindow>
#include <QObject>
#include <QPersistentModelIndex>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QTimer>
#include <QHeaderView>
#include <QVariant>
#include <Qt>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

class QAction;
class QAbstractItemModel;
class QDateTime;
class QEvent;
class QFile;
class QListWidgetItem;
class QMenu;
class QModelIndex;
class QPoint;
class QProgressDialog;
class QTranslator;
class QTreeWidgetItem;
class QUrl;
class QWidget;

#ifndef Q_MOC_RUN
#include <boost/signals2.hpp>
#endif

//Sigh - just for HANDLE
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace Ui {
    class MainWindow;
}

class Settings;


class MainWindow : public QMainWindow, public IUserInterface
{
  Q_OBJECT

  friend class OrganizerProxy;

public:
  explicit MainWindow(Settings &settings,
                      OrganizerCore &organizerCore, PluginContainer &pluginContainer,
                      QWidget *parent = 0);
  ~MainWindow();

  void processUpdates(Settings& settings);

  QWidget* qtWidget() override;

  bool addProfile();
  void updateBSAList(const QStringList &defaultArchives, const QStringList &activeArchives);
  void refreshDataTree();
  void refreshDataTreeKeepExpandedNodes();
  void refreshSaveList();

  void setModListSorting(int index);
  void setESPListSorting(int index);

  void saveArchiveList();

  void registerPluginTool(MOBase::IPluginTool *tool, QString name = QString(), QMenu *menu = nullptr);
  void registerPluginTools(std::vector<MOBase::IPluginTool *> toolPlugins);
  void registerModPage(MOBase::IPluginModPage *modPage);

  void addPrimaryCategoryCandidates(QMenu *primaryCategoryMenu, ModInfo::Ptr info);

  void createStdoutPipe(HANDLE *stdOutRead, HANDLE *stdOutWrite);
  std::string readFromPipe(HANDLE stdOutRead);
  void processLOOTOut(const std::string &lootOut, std::string &errorMessages, QProgressDialog &dialog);

  void updateModInDirectoryStructure(unsigned int index, ModInfo::Ptr modInfo);

  QString getOriginDisplayName(int originID);

  void installTranslator(const QString &name);

  virtual void disconnectPlugins();

  void displayModInformation(
    ModInfo::Ptr modInfo, unsigned int modIndex, ModInfoTabIDs tabID) override;

  bool canExit();
  void onBeforeClose();

  virtual bool closeWindow();
  virtual void setWindowEnabled(bool enabled);

  virtual MOBase::DelayedFileWriterBase &archivesWriter() override { return m_ArchiveListWriter; }

  ModInfo::Ptr nextModInList();
  ModInfo::Ptr previousModInList();

public slots:

  void displayColumnSelection(const QPoint &pos);

  void modorder_changed();
  void esplist_changed();
  void refresher_progress(int percent);
  void directory_refreshed();

  void toolPluginInvoke();
  void modPagePluginInvoke();

signals:

  /**
   * @brief emitted after the information dialog has been closed
   */
  void modInfoDisplayed();

  /**
   * @brief emitted when the selected style changes
   */
  void styleChanged(const QString &styleFile);


  void modListDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight);

protected:

  virtual void showEvent(QShowEvent *event);
  virtual void closeEvent(QCloseEvent *event);
  virtual bool eventFilter(QObject *obj, QEvent *event);
  virtual void resizeEvent(QResizeEvent *event);
  virtual void dragEnterEvent(QDragEnterEvent *event);
  virtual void dropEvent(QDropEvent *event);
  void keyReleaseEvent(QKeyEvent *event) override;

private slots:
  void on_actionChange_Game_triggered();

private slots:
  void on_clickBlankButton_clicked();

private:

  void cleanup();

  void setupToolbar();
  void setupActionMenu(QAction* a);
  void createHelpMenu();
  void createEndorseMenu();

  void updatePinnedExecutables();
  void setToolbarSize(const QSize& s);
  void setToolbarButtonStyle(Qt::ToolButtonStyle s);

  void updateToolbarMenu();
  void updateViewMenu();

  QMenu* createPopupMenu() override;
  void activateSelectedProfile();

  void startSteam();

  void updateTo(QTreeWidgetItem *subTree, const std::wstring &directorySoFar, const MOShared::DirectoryEntry &directoryEntry, bool conflictsOnly, QIcon *fileIcon, QIcon *folderIcon);
  bool refreshProfiles(bool selectProfile = true);
  void refreshExecutablesList();
  void installMod(QString fileName = "");

  QList<MOBase::IOrganizer::FileInfo> findFileInfos(const QString &path, const std::function<bool (const MOBase::IOrganizer::FileInfo &)> &filter) const;

  bool modifyExecutablesDialog(int selection);
  void displayModInformation(int row, ModInfoTabIDs tab=ModInfoTabIDs::None);
  void testExtractBSA(int modIndex);

  void writeDataToFile(QFile &file, const QString &directory, const MOShared::DirectoryEntry &directoryEntry);

  void refreshFilters();

  /**
   * Sets category selections from menu; for multiple mods, this will only apply
   * the changes made in the menu (which is the delta between the current menu selection and the reference mod)
   * @param menu the menu after editing by the user
   * @param modRow index of the mod to edit
   * @param referenceRow row of the reference mod
   */
  void addRemoveCategoriesFromMenu(QMenu *menu, int modRow, int referenceRow);

  /**
   * Sets category selections from menu; for multiple mods, this will completely
   * replace the current set of categories on each selected with those selected in the menu
   * @param menu the menu after editing by the user
   * @param modRow index of the mod to edit
   */
  void replaceCategoriesFromMenu(QMenu *menu, int modRow);

  bool populateMenuCategories(QMenu *menu, int targetID);

  void initDownloadView();
  void updateDownloadView();

  // remove invalid category-references from mods
  void fixCategories();

  bool extractProgress(QProgressDialog &extractProgress, int percentage, std::string fileName);

  size_t checkForProblems();

  QTreeWidgetItem *addFilterItem(QTreeWidgetItem *root, const QString &name, int categoryID, ModListSortProxy::FilterType type);
  void addContentFilters();
  void addCategoryFilters(QTreeWidgetItem *root, const std::set<int> &categoriesUsed, int targetID);

  void setCategoryListVisible(bool visible);

  void displaySaveGameInfo(QListWidgetItem *newItem);

  HANDLE nextChildProcess();

  bool errorReported(QString &logFile);

  void updateESPLock(bool locked);

  static void setupNetworkProxy(bool activate);
  void activateProxy(bool activate);

  bool createBackup(const QString &filePath, const QDateTime &time);
  QString queryRestore(const QString &filePath);

  void initModListContextMenu(QMenu *menu);
  void addModSendToContextMenu(QMenu *menu);
  void addPluginSendToContextMenu(QMenu *menu);

  QMenu *openFolderMenu();

  std::set<QString> enabledArchives();

  void scheduleUpdateButton();

  QDir currentSavesDir() const;

  void startMonitorSaves();
  void stopMonitorSaves();

  void dropLocalFile(const QUrl &url, const QString &outputDir, bool move);

  void sendSelectedModsToPriority(int newPriority);
  void sendSelectedPluginsToPriority(int newPriority);

  void toggleMO2EndorseState();
  void toggleUpdateAction();

private:

  static const char *PATTERN_BACKUP_GLOB;
  static const char *PATTERN_BACKUP_REGEX;
  static const char *PATTERN_BACKUP_DATE;

private:

  Ui::MainWindow *ui;

  bool m_WasVisible;

  // last separator on the toolbar, used to add spacer for right-alignment and
  // as an insert point for executables
  QAction* m_linksSeparator;

  MOBase::TutorialControl m_Tutorial;

  int m_OldProfileIndex;

  std::vector<QString> m_ModNameList; // the mod-list to go with the directory structure

  QStringList m_DefaultArchives;

  ModListSortProxy *m_ModListSortProxy;

  PluginListSortProxy *m_PluginListSortProxy;

  int m_OldExecutableIndex;

  int m_ContextRow;
  QPersistentModelIndex m_ContextIdx;
  QTreeWidgetItem *m_ContextItem;
  QAction *m_ContextAction;

  QAction* m_browseModPage;

  CategoryFactory &m_CategoryFactory;

  QTimer m_CheckBSATimer;
  QTimer m_SaveMetaTimer;
  QTimer m_UpdateProblemsTimer;

  QFuture<void> m_MetaSave;

  QTime m_StartTime;
  //SaveGameInfoWidget *m_CurrentSaveView;
  MOBase::ISaveGameInfoWidget *m_CurrentSaveView;

  OrganizerCore &m_OrganizerCore;
  PluginContainer &m_PluginContainer;

  QString m_CurrentLanguage;
  std::vector<QTranslator*> m_Translators;

  BrowserDialog m_IntegratedBrowser;

  QFileSystemWatcher m_SavesWatcher;

  std::vector<QTreeWidgetItem*> m_RemoveWidget;

  QByteArray m_ArchiveListHash;

  bool m_DidUpdateMasterList;

  bool m_showArchiveData{ true };

  MOBase::DelayedFileWriter m_ArchiveListWriter;

  QAction* m_LinkToolbar;
  QAction* m_LinkDesktop;
  QAction* m_LinkStartMenu;

  // icon set by the stylesheet, used to remember its original appearance
  // when painting the count
  QIcon m_originalNotificationIcon;

  Executable* getSelectedExecutable();

private slots:

  void updateWindowTitle(const APIUserAccount& user);
  void showMessage(const QString &message);
  void showError(const QString &message);


  // main window actions
  void helpTriggered();
  void issueTriggered();
  void wikiTriggered();
  void discordTriggered();
  void tutorialTriggered();
  void extractBSATriggered();

  //modlist shortcuts
  void openExplorer_activated();
  void refreshProfile_activated();

  // modlist context menu
  void installMod_clicked();
  void createEmptyMod_clicked();
  void createSeparator_clicked();
  void restoreBackup_clicked();
  void renameMod_clicked();
  void removeMod_clicked();
  void setColor_clicked();
  void resetColor_clicked();
  void backupMod_clicked();
  void reinstallMod_clicked();
  void endorse_clicked();
  void dontendorse_clicked();
  void unendorse_clicked();
  void track_clicked();
  void untrack_clicked();
  void ignoreMissingData_clicked();
  void markConverted_clicked();
  void visitOnNexus_clicked();
  void visitWebPage_clicked();
  void openExplorer_clicked();
  void openPluginOriginExplorer_clicked();
  void openOriginInformation_clicked();
  void information_clicked();
  void enableSelectedMods_clicked();
  void disableSelectedMods_clicked();
  void sendSelectedModsToTop_clicked();
  void sendSelectedModsToBottom_clicked();
  void sendSelectedModsToPriority_clicked();
  void sendSelectedModsToSeparator_clicked();
  // savegame context menu
  void deleteSavegame_clicked();
  void fixMods_clicked(SaveGameInfo::MissingAssets const &missingAssets);
  // data-tree context menu
  void writeDataToFile();
  void openDataFile();
  void addAsExecutable();
  void previewDataFile();
  void hideFile();
  void unhideFile();
  void openDataOriginExplorer_clicked();

  // pluginlist context menu
  void enableSelectedPlugins_clicked();
  void disableSelectedPlugins_clicked();
  void sendSelectedPluginsToTop_clicked();
  void sendSelectedPluginsToBottom_clicked();
  void sendSelectedPluginsToPriority_clicked();

  void linkToolbar();
  void linkDesktop();
  void linkMenu();

  void languageChange(const QString &newLanguage);
  void saveSelectionChanged(QListWidgetItem *newItem);

  void windowTutorialFinished(const QString &windowName);

  BSA::EErrorCode extractBSA(BSA::Archive &archive, BSA::Folder::Ptr folder, const QString &destination, QProgressDialog &extractProgress);

  void createModFromOverwrite();
  /**
   * @brief sends the content of the overwrite folder to an already existing mod
   */
  void moveOverwriteContentToExistingMod();
  /**
   * @brief actually sends the content of the overwrite folder to specified mod
   */
  void doMoveOverwriteContentToMod(const QString &modAbsolutePath);
  void clearOverwrite();

  // nexus related
  void checkModsForUpdates();

  void linkClicked(const QString &url);

  void updateAvailable();

  void actionEndorseMO();
  void actionWontEndorseMO();

  void motdReceived(const QString &motd);

  void originModified(int originID);

  void addRemoveCategories_MenuHandler();
  void replaceCategories_MenuHandler();

  void addPrimaryCategoryCandidates();

  void modInstalled(const QString &modName);

  void modUpdateCheck(std::multimap<QString, int> IDs);

  void finishUpdateInfo();

  void nxmEndorsementsAvailable(QVariant userData, QVariant resultData, int);
  void nxmUpdateInfoAvailable(QString gameName, QVariant userData, QVariant resultData, int requestID);
  void nxmUpdatesAvailable(QString gameName, int modID, QVariant userData, QVariant resultData, int requestID);
  void nxmModInfoAvailable(QString gameName, int modID, QVariant userData, QVariant resultData, int requestID);
  void nxmEndorsementToggled(QString, int, QVariant, QVariant resultData, int);
  void nxmTrackedModsAvailable(QVariant userData, QVariant resultData, int);
  void nxmDownloadURLs(QString, int modID, int fileID, QVariant userData, QVariant resultData, int requestID);
  void nxmRequestFailed(QString gameName, int modID, int fileID, QVariant userData, int requestID, QNetworkReply::NetworkError error, const QString &errorString);

  void onRequestsChanged(const APIStats& stats, const APIUserAccount& user);

  void editCategories();
  void deselectFilters();

  void displayModInformation(const QString &modName, ModInfoTabIDs tabID);

  void modRenamed(const QString &oldName, const QString &newName);
  void modRemoved(const QString &fileName);

  void hideSaveGameInfo();

  void hookUpWindowTutorials();

  void resumeDownload(int downloadIndex);
  void endorseMod(ModInfo::Ptr mod);
  void unendorseMod(ModInfo::Ptr mod);
  void trackMod(ModInfo::Ptr mod, bool doTrack);
  void cancelModListEditor();

  void lockESPIndex();
  void unlockESPIndex();

  void enableVisibleMods();
  void disableVisibleMods();
  void exportModListCSV();
  void openInstanceFolder();
  void openLogsFolder();
  void openInstallFolder();
	void openPluginsFolder();
  void openDownloadsFolder();
  void openModsFolder();
  void openProfileFolder();
  void openIniFolder();
  void openGameFolder();
  void openMyGamesFolder();
  void startExeAction();

  void checkBSAList();

  void updateProblemsButton();

  void saveModMetas();

  void updateStyle(const QString &style);

  void modlistChanged(const QModelIndex &index, int role);
  void modlistChanged(const QModelIndexList &indicies, int role);
  void fileMoved(const QString &filePath, const QString &oldOriginName, const QString &newOriginName);


  void modFilterActive(bool active);
  void espFilterChanged(const QString &filter);
  void downloadFilterChanged(const QString &filter);

  void expandModList(const QModelIndex &index);

  void resizeLists(bool pluginListCustom);

  /**
   * @brief allow columns in mod list and plugin list to be resized
   */
  void allowListResize();

  void toolBar_customContextMenuRequested(const QPoint &point);
  void removeFromToolbar();
  void overwriteClosed(int);

  void changeVersioningScheme();
  void checkModUpdates_clicked();
  void ignoreUpdate();
  void unignoreUpdate();

  void refreshSavesIfOpen();
  void expandDataTreeItem(QTreeWidgetItem *item);
  void about();
  void delayedRemove();

  void modListSortIndicatorChanged(int column, Qt::SortOrder order);
  void modListSectionResized(int logicalIndex, int oldSize, int newSize);

  void modlistSelectionsChanged(const QItemSelection &current);
  void esplistSelectionsChanged(const QItemSelection &current);

  void search_activated();
  void searchClear_activated();

  void resetActionIcons();
  void updateModCount();
  void updatePluginCount();

private slots: // ui slots
  // actions
  void on_actionAdd_Profile_triggered();
  void on_actionInstallMod_triggered();
  void on_actionModify_Executables_triggered();
  void on_actionNexus_triggered();
  void on_actionNotifications_triggered();
  void on_actionSettings_triggered();
  void on_actionUpdate_triggered();
  void on_actionExit_triggered();
  void on_actionMainMenuToggle_triggered();
  void on_actionToolBarMainToggle_triggered();
  void on_actionStatusBarToggle_triggered();
  void on_actionToolBarSmallIcons_triggered();
  void on_actionToolBarMediumIcons_triggered();
  void on_actionToolBarLargeIcons_triggered();
  void on_actionToolBarIconsOnly_triggered();
  void on_actionToolBarTextOnly_triggered();
  void on_actionToolBarIconsAndText_triggered();
  void on_actionViewLog_triggered();

  void on_centralWidget_customContextMenuRequested(const QPoint &pos);
  void on_bsaList_customContextMenuRequested(const QPoint &pos);
  void on_clearFiltersButton_clicked();
  void on_btnRefreshData_clicked();
  void on_btnRefreshDownloads_clicked();
  void on_categoriesList_customContextMenuRequested(const QPoint &pos);
  void on_conflictsCheckBox_toggled(bool checked);
  void on_showArchiveDataCheckBox_toggled(bool checked);
  void on_dataTree_customContextMenuRequested(const QPoint &pos);
  void on_executablesListBox_currentIndexChanged(int index);
  void on_modList_customContextMenuRequested(const QPoint &pos);
  void on_modList_doubleClicked(const QModelIndex &index);
  void on_listOptionsBtn_pressed();
  void on_espList_doubleClicked(const QModelIndex &index);
  void on_profileBox_currentIndexChanged(int index);
  void on_savegameList_customContextMenuRequested(const QPoint &pos);
  void on_startButton_clicked();
  void on_tabWidget_currentChanged(int index);

  void on_espList_customContextMenuRequested(const QPoint &pos);
  void on_displayCategoriesBtn_toggled(bool checked);
  void on_groupCombo_currentIndexChanged(int index);
  void on_categoriesList_itemSelectionChanged();
  void on_linkButton_pressed();
  void on_showHiddenBox_toggled(bool checked);
  void on_bsaList_itemChanged(QTreeWidgetItem *item, int column);
  void on_bossButton_clicked();

  void on_saveButton_clicked();
  void on_restoreButton_clicked();
  void on_restoreModsButton_clicked();
  void on_saveModsButton_clicked();
  void on_categoriesAndBtn_toggled(bool checked);
  void on_categoriesOrBtn_toggled(bool checked);
  void on_managedArchiveLabel_linkHovered(const QString &link);

  void storeSettings();
  void readSettings();
  void setupModList();
};

#endif // MAINWINDOW_H
