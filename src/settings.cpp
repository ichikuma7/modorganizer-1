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

#include "settings.h"
#include "settingsutilities.h"
#include "serverinfo.h"
#include "executableslist.h"
#include "appconfig.h"
#include "env.h"
#include "envmetrics.h"
#include <expanderwidget.h>
#include <utility.h>
#include <iplugingame.h>

using namespace MOBase;


EndorsementState endorsementStateFromString(const QString& s)
{
  if (s == "Endorsed") {
    return EndorsementState::Accepted;
  } else if (s == "Abstained") {
    return EndorsementState::Refused;
  } else {
    return EndorsementState::NoDecision;
  }
}

QString toString(EndorsementState s)
{
  switch (s)
  {
  case EndorsementState::Accepted:
    return "Endorsed";

  case EndorsementState::Refused:
    return "Abstained";

  case EndorsementState::NoDecision: // fall-through
  default:
    return {};
  }
}


Settings *Settings::s_Instance = nullptr;

Settings::Settings(const QString& path) :
  m_Settings(path, QSettings::IniFormat),
  m_Game(m_Settings), m_Geometry(m_Settings), m_Widgets(m_Settings),
  m_Colors(m_Settings), m_Plugins(m_Settings), m_Paths(m_Settings),
  m_Network(m_Settings), m_Nexus(*this, m_Settings), m_Steam(*this, m_Settings),
  m_Interface(m_Settings), m_Diagnostics(m_Settings)
{
  if (s_Instance != nullptr) {
    throw std::runtime_error("second instance of \"Settings\" created");
  } else {
    s_Instance = this;
  }
}

Settings::~Settings()
{
  MOBase::QuestionBoxMemory::setCallbacks({}, {}, {});
  s_Instance = nullptr;
}

Settings &Settings::instance()
{
  if (s_Instance == nullptr) {
    throw std::runtime_error("no instance of \"Settings\"");
  }
  return *s_Instance;
}

void Settings::processUpdates(
  const QVersionNumber& currentVersion, const QVersionNumber& lastVersion)
{
  if (firstStart()) {
    return;
  }

  if (currentVersion == lastVersion) {
    return;
  }

  log::info(
    "updating from {} to {}",
    lastVersion.toString(), currentVersion.toString());

  auto version = [&](const QVersionNumber& v, auto&& f) {
    if (lastVersion < v) {
      log::debug("processing updates for {}", v.toString());
      f();
    }
  };

  version({2, 2, 0}, [&]{
    remove(m_Settings, "Settings", "steam_password");
    remove(m_Settings, "Settings", "nexus_username");
    remove(m_Settings, "Settings", "nexus_password");
    remove(m_Settings, "Settings", "nexus_login");
    remove(m_Settings, "Settings", "nexus_api_key");
    remove(m_Settings, "Settings", "ask_for_nexuspw");
    remove(m_Settings, "Settings", "nmm_version");

    removeSection(m_Settings, "Servers");
  });

  version({2, 2, 1}, [&]{
    remove(m_Settings, "General", "mod_info_tabs");
    remove(m_Settings, "General", "mod_info_conflict_expanders");
    remove(m_Settings, "General", "mod_info_conflicts");
    remove(m_Settings, "General", "mod_info_advanced_conflicts");
    remove(m_Settings, "General", "mod_info_conflicts_overwrite");
    remove(m_Settings, "General", "mod_info_conflicts_noconflict");
    remove(m_Settings, "General", "mod_info_conflicts_overwritten");
  });

  version({2, 2, 2}, [&]{
    // log splitter is gone, it's a dock now
    remove(m_Settings, "General", "log_split");

    // moved to widgets
    remove(m_Settings, "General", "mod_info_conflicts_tab");
    remove(m_Settings, "General", "mod_info_conflicts_general_expanders");
    remove(m_Settings, "General", "mod_info_conflicts_general_overwrite");
    remove(m_Settings, "General", "mod_info_conflicts_general_noconflict");
    remove(m_Settings, "General", "mod_info_conflicts_general_overwritten");
    remove(m_Settings, "General", "mod_info_conflicts_advanced_list");
    remove(m_Settings, "General", "mod_info_conflicts_advanced_options");
    remove(m_Settings, "General", "mod_info_tab_order");
    remove(m_Settings, "General", "mod_info_dialog_images_show_dds");

    // moved to geometry
    remove(m_Settings, "General", "window_geometry");
    remove(m_Settings, "General", "window_state");
    remove(m_Settings, "General", "toolbar_size");
    remove(m_Settings, "General", "toolbar_button_style");
    remove(m_Settings, "General", "menubar_visible");
    remove(m_Settings, "General", "statusbar_visible");
    remove(m_Settings, "General", "window_split");
    remove(m_Settings, "General", "window_monitor");
    remove(m_Settings, "General", "browser_geometry");
    remove(m_Settings, "General", "filters_visible");

    // this was supposed to have been removed above when updating from 2.2.0,
    // but it wasn't in Settings, it was in General
    remove(m_Settings, "General", "ask_for_nexuspw");

    m_Network.updateFromOldMap();
  });

  //save version in all case
  set(m_Settings, "General", "version", currentVersion.toString());

  log::debug("updating done");
}

QString Settings::filename() const
{
  return m_Settings.fileName();
}

bool Settings::checkForUpdates() const
{
  return get<bool>(m_Settings, "Settings", "check_for_updates", true);
}

void Settings::setCheckForUpdates(bool b)
{
  set(m_Settings, "Settings", "check_for_updates", b);
}

bool Settings::usePrereleases() const
{
  return get<bool>(m_Settings, "Settings", "use_prereleases", false);
}

void Settings::setUsePrereleases(bool b)
{
  set(m_Settings, "Settings", "use_prereleases", b);
}

std::optional<QVersionNumber> Settings::version() const
{
  if (auto v=getOptional<QString>(m_Settings, "General", "version")) {
    return QVersionNumber::fromString(*v).normalized();
  }

  return {};
}

bool Settings::firstStart() const
{
  return get<bool>(m_Settings, "General", "first_start", true);
}

void Settings::setFirstStart(bool b)
{
  set(m_Settings, "General", "first_start", b);
}

QString Settings::executablesBlacklist() const
{
  static const QString def = (QStringList()
    << "Chrome.exe"
    << "Firefox.exe"
    << "TSVNCache.exe"
    << "TGitCache.exe"
    << "Steam.exe"
    << "GameOverlayUI.exe"
    << "Discord.exe"
    << "GalaxyClient.exe"
    << "Spotify.exe"
    ).join(";");

  return get<QString>(m_Settings, "Settings", "executable_blacklist", def);
}

bool Settings::isExecutableBlacklisted(const QString& s) const
{
  for (auto exec : executablesBlacklist().split(";")) {
    if (exec.compare(s, Qt::CaseInsensitive) == 0) {
      return true;
    }
  }

  return false;
}

void Settings::setExecutablesBlacklist(const QString& s)
{
  set(m_Settings, "Settings", "executable_blacklist", s);
}

void Settings::setMotdHash(uint hash)
{
  set(m_Settings, "General", "motd_hash", hash);
}

unsigned int Settings::motdHash() const
{
  return get<unsigned int>(m_Settings, "General", "motd_hash", 0);
}

bool Settings::archiveParsing() const
{
  return get<bool>(m_Settings, "Settings", "archive_parsing_experimental", false);
}

void Settings::setArchiveParsing(bool b)
{
  set(m_Settings, "Settings", "archive_parsing_experimental", b);
}

std::vector<std::map<QString, QVariant>> Settings::executables() const
{
  ScopedReadArray sra(m_Settings, "customExecutables");
  std::vector<std::map<QString, QVariant>> v;

  sra.for_each([&]{
    std::map<QString, QVariant> map;

    for (auto&& key : sra.keys()) {
      map[key] = sra.get<QVariant>(key);
    }

    v.push_back(map);
  });

  return v;
}

void Settings::setExecutables(const std::vector<std::map<QString, QVariant>>& v)
{
  const auto current = executables();

  if (current == v) {
    // no change
    return;
  }

  if (current.size() > v.size()) {
    // Qt can't remove array elements, the section must be cleared
    removeSection(m_Settings, "customExecutables");
  }

  ScopedWriteArray swa(m_Settings, "customExecutables", v.size());

  for (const auto& map : v) {
    swa.next();

    for (auto&& p : map) {
      swa.set(p.first, p.second);
    }
  }
}

bool Settings::keepBackupOnInstall() const
{
  return get<bool>(m_Settings, "General", "backup_install", false);
}

void Settings::setKeepBackupOnInstall(bool b)
{
  set(m_Settings, "General", "backup_install", b);
}

GameSettings& Settings::game()
{
  return m_Game;
}

const GameSettings& Settings::game() const
{
  return m_Game;
}

GeometrySettings& Settings::geometry()
{
  return m_Geometry;
}

const GeometrySettings& Settings::geometry() const
{
  return m_Geometry;
}

WidgetSettings& Settings::widgets()
{
  return m_Widgets;
}

const WidgetSettings& Settings::widgets() const
{
  return m_Widgets;
}

ColorSettings& Settings::colors()
{
  return m_Colors;
}

const ColorSettings& Settings::colors() const
{
  return m_Colors;
}

PluginSettings& Settings::plugins()
{
  return m_Plugins;
}

const PluginSettings& Settings::plugins() const
{
  return m_Plugins;
}

PathSettings& Settings::paths()
{
  return m_Paths;
}

const PathSettings& Settings::paths() const
{
  return m_Paths;
}

NetworkSettings& Settings::network()
{
  return m_Network;
}

const NetworkSettings& Settings::network() const
{
  return m_Network;
}

NexusSettings& Settings::nexus()
{
  return m_Nexus;
}

const NexusSettings& Settings::nexus() const
{
  return m_Nexus;
}

SteamSettings& Settings::steam()
{
  return m_Steam;
}

const SteamSettings& Settings::steam() const
{
  return m_Steam;
}

InterfaceSettings& Settings::interface()
{
  return m_Interface;
}

const InterfaceSettings& Settings::interface() const
{
  return m_Interface;
}

DiagnosticsSettings& Settings::diagnostics()
{
  return m_Diagnostics;
}

const DiagnosticsSettings& Settings::diagnostics() const
{
  return m_Diagnostics;
}

QSettings::Status Settings::sync() const
{
  m_Settings.sync();
  return m_Settings.status();
}

void Settings::dump() const
{
  static const QStringList ignore({
    "username", "password", "nexus_api_key"
    });

  log::debug("settings:");

  {
    ScopedGroup sg(m_Settings, "Settings");

    for (auto k : m_Settings.allKeys()) {
      if (ignore.contains(k, Qt::CaseInsensitive)) {
        continue;
      }

      log::debug("  . {}={}", k, m_Settings.value(k).toString());
    }
  }

  m_Network.dump();
}

void Settings::managedGameChanged(IPluginGame const *gamePlugin)
{
  m_Game.setPlugin(gamePlugin);
}


GameSettings::GameSettings(QSettings& settings)
  : m_Settings(settings), m_GamePlugin(nullptr)
{
}

const MOBase::IPluginGame* GameSettings::plugin()
{
  return m_GamePlugin;
}

void GameSettings::setPlugin(const MOBase::IPluginGame* gamePlugin)
{
  m_GamePlugin = gamePlugin;
}

bool GameSettings::forceEnableCoreFiles() const
{
  return get<bool>(m_Settings, "Settings", "force_enable_core_files", true);
}

void GameSettings::setForceEnableCoreFiles(bool b)
{
  set(m_Settings, "Settings", "force_enable_core_files", b);
}

std::optional<QString> GameSettings::directory() const
{
  if (auto v=getOptional<QByteArray>(m_Settings, "General", "gamePath")) {
    return QString::fromUtf8(*v);
  }

  return {};
}

void GameSettings::setDirectory(const QString& path)
{
  set(m_Settings, "General", "gamePath", QDir::toNativeSeparators(path).toUtf8());
}

std::optional<QString> GameSettings::name() const
{
  return getOptional<QString>(m_Settings, "General", "gameName");
}

void GameSettings::setName(const QString& name)
{
  set(m_Settings, "General", "gameName", name);
}

std::optional<QString> GameSettings::edition() const
{
  return getOptional<QString>(m_Settings, "General", "game_edition");
}

void GameSettings::setEdition(const QString& name)
{
  set(m_Settings, "General", "game_edition", name);
}

std::optional<QString> GameSettings::selectedProfileName() const
{
  if (auto v=getOptional<QByteArray>(m_Settings, "General", "selected_profile")) {
    return QString::fromUtf8(*v);
  }

  return {};
}

void GameSettings::setSelectedProfileName(const QString& name)
{
  set(m_Settings, "General", "selected_profile", name.toUtf8());
}

LoadMechanism::EMechanism GameSettings::loadMechanismType() const
{
  const auto def = LoadMechanism::LOAD_MODORGANIZER;

  const auto i = get<LoadMechanism::EMechanism>(m_Settings,
    "Settings", "load_mechanism", def);

  switch (i)
  {
    // ok
    case LoadMechanism::LOAD_MODORGANIZER:  // fall-through
    {
      break;
    }

    default:
    {
      log::error(
        "invalid load mechanism {}, reverting to {}",
        static_cast<int>(i), toString(def));

      set(m_Settings, "Settings", "load_mechanism", def);

      return def;
    }
  }

  return i;
}

void GameSettings::setLoadMechanism(LoadMechanism::EMechanism m)
{
  set(m_Settings, "Settings", "load_mechanism", m);
}

const LoadMechanism& GameSettings::loadMechanism() const
{
  return m_LoadMechanism;
}

LoadMechanism& GameSettings::loadMechanism()
{
  return m_LoadMechanism;
}

bool GameSettings::hideUncheckedPlugins() const
{
  return get<bool>(m_Settings, "Settings", "hide_unchecked_plugins", false);
}

void GameSettings::setHideUncheckedPlugins(bool b)
{
  set(m_Settings, "Settings", "hide_unchecked_plugins", b);
}


GeometrySettings::GeometrySettings(QSettings& s)
  : m_Settings(s), m_Reset(false)
{
}

void GeometrySettings::requestReset()
{
  m_Reset = true;
}

void GeometrySettings::resetIfNeeded()
{
  if (!m_Reset) {
    return;
  }

  removeSection(m_Settings, "Geometry");
}

void GeometrySettings::saveGeometry(const QMainWindow* w)
{
  saveWindowGeometry(w);
}

bool GeometrySettings::restoreGeometry(QMainWindow* w) const
{
  return restoreWindowGeometry(w);
}

void GeometrySettings::saveGeometry(const QDialog* d)
{
  saveWindowGeometry(d);
}

bool GeometrySettings::restoreGeometry(QDialog* d) const
{
  const auto r = restoreWindowGeometry(d);

  if (centerDialogs()) {
    centerOnParent(d);
  }

  return r;
}

void GeometrySettings::saveWindowGeometry(const QWidget* w)
{
  set(m_Settings, "Geometry", geoSettingName(w), w->saveGeometry());
}

bool GeometrySettings::restoreWindowGeometry(QWidget* w) const
{
  if (auto v=getOptional<QByteArray>(m_Settings, "Geometry", geoSettingName(w))) {
    w->restoreGeometry(*v);
    ensureWindowOnScreen(w);
    return true;
  }

  return false;
}

void GeometrySettings::ensureWindowOnScreen(QWidget* w) const
{
  // users report that the main window and/or dialogs are displayed off-screen;
  // the usual workaround is keyboard navigation to move it
  //
  // qt should have code that deals with multiple monitors and off-screen
  // geometries, but there seems to be bugs or inconsistencies that can't be
  // reproduced
  //
  // the closest would probably be https://bugreports.qt.io/browse/QTBUG-64498,
  // which is about multiple monitors and high dpi, but it seems fixed as of
  // 5.12.4, which is shipped with 2.2.1
  //
  // without being to reproduce the problem, some simple checks are made in a
  // timer, which may mitigate the issues

  QTimer::singleShot(100, w, [w] {
    const auto borders = 20;

    // desktop geometry, made smaller to make sure there isn't just a few pixels
    const auto originalDg = env::Environment().metrics().desktopGeometry();
    const auto dg = originalDg.adjusted(borders, borders, -borders, -borders);

    const auto g = w->geometry();

    if (!dg.intersects(g)) {
      log::warn(
        "window '{}' is offscreen, moving to main monitor; geo={}, desktop={}",
        w->objectName(), g, originalDg);

      // widget is off-screen, center it on main monitor
      centerOnMonitor(w, -1);

      log::warn("window '{}' now at {}", w->objectName(), w->geometry());
    }
  });
}

void GeometrySettings::saveState(const QMainWindow* w)
{
  set(m_Settings, "Geometry", stateSettingName(w), w->saveState());
}

bool GeometrySettings::restoreState(QMainWindow* w) const
{
  if (auto v=getOptional<QByteArray>(m_Settings, "Geometry", stateSettingName(w))) {
    w->restoreState(*v);
    return true;
  }

  return false;
}

void GeometrySettings::saveState(const QHeaderView* w)
{
  set(m_Settings, "Geometry", stateSettingName(w), w->saveState());
}

bool GeometrySettings::restoreState(QHeaderView* w) const
{
  if (auto v=getOptional<QByteArray>(m_Settings, "Geometry", stateSettingName(w))) {
    w->restoreState(*v);
    return true;
  }

  return false;
}

void GeometrySettings::saveState(const QSplitter* w)
{
  set(m_Settings, "Geometry", stateSettingName(w), w->saveState());
}

bool GeometrySettings::restoreState(QSplitter* w) const
{
  if (auto v=getOptional<QByteArray>(m_Settings, "Geometry", stateSettingName(w))) {
    w->restoreState(*v);
    return true;
  }

  return false;
}

void GeometrySettings::saveState(const ExpanderWidget* expander)
{
  set(m_Settings, "Geometry", stateSettingName(expander), expander->saveState());
}

bool GeometrySettings::restoreState(ExpanderWidget* expander) const
{
  if (auto v=getOptional<QByteArray>(m_Settings, "Geometry", stateSettingName(expander))) {
    expander->restoreState(*v);
    return true;
  }

  return false;
}

void GeometrySettings::saveVisibility(const QWidget* w)
{
  set(m_Settings, "Geometry", visibilitySettingName(w), w->isVisible());
}

bool GeometrySettings::restoreVisibility(QWidget* w, std::optional<bool> def) const
{
  if (auto v=getOptional<bool>(m_Settings, "Geometry", visibilitySettingName(w), def)) {
    w->setVisible(*v);
    return true;
  }

  return false;
}

void GeometrySettings::restoreToolbars(QMainWindow* w) const
{
  // all toolbars have the same size and button style settings
  const auto size = getOptional<QSize>(m_Settings, "Geometry", "toolbar_size");
  const auto style = getOptional<int>(m_Settings, "Geometry", "toolbar_button_style");

  for (auto* tb : w->findChildren<QToolBar*>()) {
    if (size) {
      tb->setIconSize(*size);
    }

    if (style) {
      tb->setToolButtonStyle(static_cast<Qt::ToolButtonStyle>(*style));
    }

    restoreVisibility(tb);
  }
}

void GeometrySettings::saveToolbars(const QMainWindow* w)
{
  const auto tbs = w->findChildren<QToolBar*>();

  // save visibility for all
  for (auto* tb : tbs) {
    saveVisibility(tb);
  }

  // all toolbars have the same size and button style settings, just save the
  // first one
  if (!tbs.isEmpty()) {
    const auto* tb = tbs[0];

    set(m_Settings, "Geometry", "toolbar_size", tb->iconSize());
    set(m_Settings, "Geometry", "toolbar_button_style", static_cast<int>(tb->toolButtonStyle()));
  }
}

QStringList GeometrySettings::modInfoTabOrder() const
{
  QStringList v;

  if (m_Settings.contains("mod_info_tabs")) {
    // old byte array from 2.2.0
    QDataStream stream(m_Settings.value("mod_info_tabs").toByteArray());

    int count = 0;
    stream >> count;

    for (int i=0; i<count; ++i) {
      QString s;
      stream >> s;
      v.push_back(s);
    }
  } else {
    // string list since 2.2.1
    QString string = get<QString>(m_Settings, "Widgets", "ModInfoTabOrder", "");
    QTextStream stream(&string);

    while (!stream.atEnd()) {
      QString s;
      stream >> s;
      v.push_back(s);
    }
  }

  return v;
}

void GeometrySettings::setModInfoTabOrder(const QString& names)
{
  set(m_Settings, "Widgets", "ModInfoTabOrder", names);
}

bool GeometrySettings::centerDialogs() const
{
  return get<bool>(m_Settings, "Settings", "center_dialogs", false);
}

void GeometrySettings::setCenterDialogs(bool b)
{
  set(m_Settings, "Settings", "center_dialogs", b);
}

void GeometrySettings::centerOnMainWindowMonitor(QWidget* w)
{
  const auto monitor = getOptional<int>(
    m_Settings, "Geometry", "MainWindow_monitor").value_or(-1);

  centerOnMonitor(w, monitor);
}

void GeometrySettings::centerOnMonitor(QWidget* w, int monitor)
{
  QPoint center;

  if (monitor >= 0 && monitor < QGuiApplication::screens().size()) {
    center = QGuiApplication::screens().at(monitor)->geometry().center();
  } else {
    center = QGuiApplication::primaryScreen()->geometry().center();
  }

  w->move(center - w->rect().center());
}

void GeometrySettings::centerOnParent(QWidget* w, QWidget* parent)
{
  if (!parent) {
    parent = w->parentWidget();

    if (!parent) {
      parent = qApp->activeWindow();
    }
  }

  if (parent && parent->isVisible()) {
    const auto pr = parent->geometry();
    w->move(pr.center() - w->rect().center());
  }
}

void GeometrySettings::saveMainWindowMonitor(const QMainWindow* w)
{
  if (auto* handle=w->windowHandle()) {
    if (auto* screen = handle->screen()) {
      const int screenId = QGuiApplication::screens().indexOf(screen);
      set(m_Settings, "Geometry", "MainWindow_monitor", screenId);
    }
  }
}

Qt::Orientation dockOrientation(const QMainWindow* mw, const QDockWidget* d)
{
  // docks in these areas are horizontal
  const auto horizontalAreas =
    Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea;

  if (mw->dockWidgetArea(const_cast<QDockWidget*>(d)) & horizontalAreas) {
    return Qt::Horizontal;
  } else {
    return Qt::Vertical;
  }
}

void GeometrySettings::saveDocks(const QMainWindow* mw)
{
  // this attempts to fix https://bugreports.qt.io/browse/QTBUG-46620 where dock
  // sizes are not restored when the main window is maximized; it is used in
  // MainWindow::readSettings() and MainWindow::storeSettings()
  //
  // there's also https://stackoverflow.com/questions/44005852, which has what
  // seems to be a popular fix, but it breaks the restored size of the window
  // by setting it to the desktop's resolution, so that doesn't work
  //
  // the only fix I could find is to remember the sizes of the docks and manually
  // setting them back; saving is straightforward, but restoring is messy
  //
  // this also depends on the window being visible before the timer in restore()
  // is fired and the timer must be processed by application.exec(); therefore,
  // the splash screen _must_ be closed before readSettings() is called, because
  // it has its own event loop, which seems to interfere with this
  //
  // all of this should become unnecessary when QTBUG-46620 is fixed
  //

  // saves the size of each dock
  for (const auto* dock : mw->findChildren<QDockWidget*>()) {
    int size = 0;

    // save the width for horizontal docks, or the height for vertical
    if (dockOrientation(mw, dock) == Qt::Horizontal) {
      size = dock->size().width();
    } else {
      size = dock->size().height();
    }

    set(m_Settings, "Geometry", dockSettingName(dock), size);
  }
}

void GeometrySettings::restoreDocks(QMainWindow* mw) const
{
  struct DockInfo
  {
    QDockWidget* d;
    int size = 0;
    Qt::Orientation ori;
  };

  std::vector<DockInfo> dockInfos;

  // for each dock
  for (auto* dock : mw->findChildren<QDockWidget*>()) {
    if (auto size=getOptional<int>(m_Settings, "Geometry", dockSettingName(dock))) {
      // remember this dock, its size and orientation
      dockInfos.push_back({dock, *size, dockOrientation(mw, dock)});
    }
  }

  // the main window must have had time to process the settings from
  // readSettings() or it seems to override whatever is set here
  //
  // some people said a single processEvents() call is enough, but it doesn't
  // look like it
  QTimer::singleShot(5, [=] {
    for (const auto& info : dockInfos) {
      mw->resizeDocks({info.d}, {info.size}, info.ori);
    }
    });
}


WidgetSettings::WidgetSettings(QSettings& s)
  : m_Settings(s)
{
  MOBase::QuestionBoxMemory::setCallbacks(
    [this](auto&& w, auto&& f){ return questionButton(w, f); },
    [this](auto&& w, auto&& b){ setQuestionWindowButton(w, b); },
    [this](auto&& w, auto&& f, auto&& b){ setQuestionFileButton(w, f, b); });
}

std::optional<int> WidgetSettings::index(const QComboBox* cb) const
{
  return getOptional<int>(m_Settings, "Widgets", indexSettingName(cb));
}

void WidgetSettings::saveIndex(const QComboBox* cb)
{
  set(m_Settings, "Widgets", indexSettingName(cb), cb->currentIndex());
}

void WidgetSettings::restoreIndex(QComboBox* cb, std::optional<int> def) const
{
  if (auto v=getOptional<int>(m_Settings, "Widgets", indexSettingName(cb), def)) {
    cb->setCurrentIndex(*v);
  }
}

std::optional<int> WidgetSettings::index(const QTabWidget* w) const
{
  return getOptional<int>(m_Settings, "Widgets", indexSettingName(w));
}

void WidgetSettings::saveIndex(const QTabWidget* w)
{
  set(m_Settings, "Widgets", indexSettingName(w), w->currentIndex());
}

void WidgetSettings::restoreIndex(QTabWidget* w, std::optional<int> def) const
{
  if (auto v=getOptional<int>(m_Settings, "Widgets", indexSettingName(w), def)) {
    w->setCurrentIndex(*v);
  }
}

std::optional<bool> WidgetSettings::checked(const QAbstractButton* w) const
{
  warnIfNotCheckable(w);
  return getOptional<bool>(m_Settings, "Widgets", checkedSettingName(w));
}

void WidgetSettings::saveChecked(const QAbstractButton* w)
{
  warnIfNotCheckable(w);
  set(m_Settings, "Widgets", checkedSettingName(w), w->isChecked());
}

void WidgetSettings::restoreChecked(QAbstractButton* w, std::optional<bool> def) const
{
  warnIfNotCheckable(w);

  if (auto v=getOptional<bool>(m_Settings, "Widgets", checkedSettingName(w), def)) {
    w->setChecked(*v);
  }
}

QuestionBoxMemory::Button WidgetSettings::questionButton(
  const QString& windowName, const QString& filename) const
{
  const QString sectionName("DialogChoices");

  if (!filename.isEmpty()) {
    const auto fileSetting = windowName + "/" + filename;
    if (auto v=getOptional<int>(m_Settings, sectionName, fileSetting)) {
      return static_cast<QuestionBoxMemory::Button>(*v);
    }
  }

  if (auto v=getOptional<int>(m_Settings, sectionName, windowName)) {
    return static_cast<QuestionBoxMemory::Button>(*v);
  }

  return QuestionBoxMemory::NoButton;
}

void WidgetSettings::setQuestionWindowButton(
  const QString& windowName, QuestionBoxMemory::Button button)
{
  const QString sectionName("DialogChoices");

  if (button == QuestionBoxMemory::NoButton) {
    remove(m_Settings, sectionName, windowName);
  } else {
    set(m_Settings, sectionName, windowName, button);
  }
}

void WidgetSettings::setQuestionFileButton(
  const QString& windowName, const QString& filename,
  QuestionBoxMemory::Button button)
{
  const QString sectionName("DialogChoices");
  const QString settingName(windowName + "/" + filename);

  if (button == QuestionBoxMemory::NoButton) {
    remove(m_Settings, sectionName, settingName);
  } else {
    set(m_Settings, sectionName, settingName, button);
  }
}

void WidgetSettings::resetQuestionButtons()
{
  removeSection(m_Settings, "DialogChoices");
}


ColorSettings::ColorSettings(QSettings& s)
  : m_Settings(s)
{
}

QColor ColorSettings::modlistOverwrittenLoose() const
{
  return get<QColor>(
    m_Settings, "Settings", "overwrittenLooseFilesColor",
    QColor(0, 255, 0, 64));
}

void ColorSettings::setModlistOverwrittenLoose(const QColor& c)
{
  set(m_Settings, "Settings", "overwrittenLooseFilesColor", c);
}

QColor ColorSettings::modlistOverwritingLoose() const
{
  return get<QColor>(
    m_Settings, "Settings", "overwritingLooseFilesColor",
    QColor(255, 0, 0, 64));
}

void ColorSettings::setModlistOverwritingLoose(const QColor& c)
{
  set(m_Settings, "Settings", "overwritingLooseFilesColor", c);
}

QColor ColorSettings::modlistOverwrittenArchive() const
{
  return get<QColor>(
    m_Settings, "Settings", "overwrittenArchiveFilesColor",
    QColor(0, 255, 255, 64));
}

void ColorSettings::setModlistOverwrittenArchive(const QColor& c)
{
  set(m_Settings, "Settings", "overwrittenArchiveFilesColor", c);
}

QColor ColorSettings::modlistOverwritingArchive() const
{
  return get<QColor>(
    m_Settings, "Settings", "overwritingArchiveFilesColor",
    QColor(255, 0, 255, 64));
}

void ColorSettings::setModlistOverwritingArchive(const QColor& c)
{
  set(m_Settings, "Settings", "overwritingArchiveFilesColor", c);
}

QColor ColorSettings::modlistContainsPlugin() const
{
  return get<QColor>(
    m_Settings, "Settings", "containsPluginColor",
    QColor(0, 0, 255, 64));
}

void ColorSettings::setModlistContainsPlugin(const QColor& c)
{
  set(m_Settings, "Settings", "containsPluginColor", c);
}

QColor ColorSettings::pluginListContained() const
{
  return get<QColor>(
    m_Settings, "Settings", "containedColor",
    QColor(0, 0, 255, 64));
}

void ColorSettings::setPluginListContained(const QColor& c)
{
  set(m_Settings, "Settings", "containedColor", c);
}

std::optional<QColor> ColorSettings::previousSeparatorColor() const
{
  const auto c = getOptional<QColor>(m_Settings, "General", "previousSeparatorColor");
  if (c && c->isValid()) {
    return c;
  }

  return {};
}

void ColorSettings::setPreviousSeparatorColor(const QColor& c) const
{
  set(m_Settings, "General", "previousSeparatorColor", c);
}

void ColorSettings::removePreviousSeparatorColor()
{
  remove(m_Settings, "General", "previousSeparatorColor");
}

bool ColorSettings::colorSeparatorScrollbar() const
{
  return get<bool>(m_Settings, "Settings", "colorSeparatorScrollbars", true);
}

void ColorSettings::setColorSeparatorScrollbar(bool b)
{
  set(m_Settings, "Settings", "colorSeparatorScrollbars", b);
}

QColor ColorSettings::idealTextColor(const QColor& rBackgroundColor)
{
  if (rBackgroundColor.alpha() == 0)
    return QColor(Qt::black);

  const int THRESHOLD = 106 * 255.0f / rBackgroundColor.alpha();
  int BackgroundDelta = (rBackgroundColor.red() * 0.299) + (rBackgroundColor.green() * 0.587) + (rBackgroundColor.blue() * 0.114);
  return QColor((255 - BackgroundDelta <= THRESHOLD) ? Qt::black : Qt::white);
}



PluginSettings::PluginSettings(QSettings& settings)
  : m_Settings(settings)
{
}

void PluginSettings::clearPlugins()
{
  m_Plugins.clear();
  m_PluginSettings.clear();
  m_PluginBlacklist.clear();

  m_PluginBlacklist = readBlacklist();
}

void PluginSettings::registerPlugin(IPlugin *plugin)
{
  m_Plugins.push_back(plugin);
  m_PluginSettings.insert(plugin->name(), QVariantMap());
  m_PluginDescriptions.insert(plugin->name(), QVariantMap());

  for (const PluginSetting &setting : plugin->settings()) {
    const QString settingName = plugin->name() + "/" + setting.key;

    QVariant temp = get<QVariant>(
      m_Settings, "Plugins", settingName, setting.defaultValue);

    if (!temp.convert(setting.defaultValue.type())) {
      log::warn(
        "failed to interpret \"{}\" as correct type for \"{}\" in plugin \"{}\", using default",
        temp.toString(), setting.key, plugin->name());

      temp = setting.defaultValue;
    }

    m_PluginSettings[plugin->name()][setting.key] = temp;

    m_PluginDescriptions[plugin->name()][setting.key] = QString("%1 (default: %2)")
      .arg(setting.description)
      .arg(setting.defaultValue.toString());
  }
}

std::vector<MOBase::IPlugin*> PluginSettings::plugins() const
{
  return m_Plugins;
}

QVariant PluginSettings::setting(const QString &pluginName, const QString &key) const
{
  auto iterPlugin = m_PluginSettings.find(pluginName);
  if (iterPlugin == m_PluginSettings.end()) {
    return QVariant();
  }

  auto iterSetting = iterPlugin->find(key);
  if (iterSetting == iterPlugin->end()) {
    return QVariant();
  }

  return *iterSetting;
}

void PluginSettings::setSetting(const QString &pluginName, const QString &key, const QVariant &value)
{
  auto iterPlugin = m_PluginSettings.find(pluginName);

  if (iterPlugin == m_PluginSettings.end()) {
    throw MyException(
      QObject::tr("attempt to store setting for unknown plugin \"%1\"")
      .arg(pluginName));
  }

  // store the new setting both in memory and in the ini
  m_PluginSettings[pluginName][key] = value;
  set(m_Settings, "Plugins", pluginName + "/" + key, value);
}

QVariantMap PluginSettings::settings(const QString &pluginName) const
{
  return m_PluginSettings[pluginName];
}

void PluginSettings::setSettings(const QString &pluginName, const QVariantMap& map)
{
  m_PluginSettings[pluginName] = map;
}

QVariantMap PluginSettings::descriptions(const QString &pluginName) const
{
  return m_PluginDescriptions[pluginName];
}

void PluginSettings::setDescriptions(const QString &pluginName, const QVariantMap& map)
{
  m_PluginDescriptions[pluginName] = map;
}

QVariant PluginSettings::persistent(const QString &pluginName, const QString &key, const QVariant &def) const
{
  if (!m_PluginSettings.contains(pluginName)) {
    return def;
  }

  return get<QVariant>(m_Settings, "PluginPersistance", pluginName + "/" + key, def);
}

void PluginSettings::setPersistent(
  const QString &pluginName, const QString &key, const QVariant &value, bool sync)
{
  if (!m_PluginSettings.contains(pluginName)) {
    throw MyException(
      QObject::tr("attempt to store setting for unknown plugin \"%1\"")
      .arg(pluginName));
  }

  set(m_Settings, "PluginPersistance", pluginName + "/" + key, value);

  if (sync) {
    m_Settings.sync();
  }
}

void PluginSettings::addBlacklist(const QString &fileName)
{
  m_PluginBlacklist.insert(fileName);
  writeBlacklist();
}

bool PluginSettings::blacklisted(const QString &fileName) const
{
  return m_PluginBlacklist.contains(fileName);
}

void PluginSettings::setBlacklist(const QStringList& pluginNames)
{
  m_PluginBlacklist.clear();

  for (const auto& name : pluginNames) {
    m_PluginBlacklist.insert(name);
  }
}

const QSet<QString>& PluginSettings::blacklist() const
{
  return m_PluginBlacklist;
}

void PluginSettings::save()
{
  for (auto iterPlugins=m_PluginSettings.begin(); iterPlugins!=m_PluginSettings.end(); ++iterPlugins) {
    for (auto iterSettings=iterPlugins->begin(); iterSettings!=iterPlugins->end(); ++iterSettings) {
      const auto key = iterPlugins.key() + "/" + iterSettings.key();
      set(m_Settings, "Plugins", key, iterSettings.value());
    }
  }

  writeBlacklist();
}

void PluginSettings::writeBlacklist()
{
  const auto current = readBlacklist();

  if (current.size() > m_PluginBlacklist.size()) {
    // Qt can't remove array elements, the section must be cleared
    removeSection(m_Settings, "pluginBlacklist");
  }

  ScopedWriteArray swa(m_Settings, "pluginBlacklist", m_PluginBlacklist.size());

  for (const QString &plugin : m_PluginBlacklist) {
    swa.next();
    swa.set("name", plugin);
  }
}

QSet<QString> PluginSettings::readBlacklist() const
{
  QSet<QString> set;

  ScopedReadArray sra(m_Settings, "pluginBlacklist");
  sra.for_each([&]{
    set.insert(sra.get<QString>("name"));
    });

  return set;
}


PathSettings::PathSettings(QSettings& settings)
  : m_Settings(settings)
{
}

std::map<QString, QString> PathSettings::recent() const
{
  std::map<QString, QString> map;

  ScopedReadArray sra(m_Settings, "recentDirectories");

  sra.for_each([&] {
    const QVariant name = sra.get<QVariant>("name");
    const QVariant dir = sra.get<QVariant>("directory");

    if (name.isValid() && dir.isValid()) {
      map.emplace(name.toString(), dir.toString());
    }
    });

  return map;
}

void PathSettings::setRecent(const std::map<QString, QString>& map)
{
  const auto current = recent();

  if (current.size() > map.size()) {
    // Qt can't remove array elements, the section must be cleared
    removeSection(m_Settings, "recentDirectories");
  }

  ScopedWriteArray swa(m_Settings, "recentDirectories", map.size());

  for (auto&& p : map) {
    swa.next();

    swa.set("name", p.first);
    swa.set("directory", p.second);
  }
}

QString PathSettings::getConfigurablePath(const QString &key,
  const QString &def,
  bool resolve) const
{
  QString result = QDir::fromNativeSeparators(
    get<QString>(m_Settings, "Settings", key, QString("%BASE_DIR%/") + def));

  if (resolve) {
    result.replace("%BASE_DIR%", base());
  }

  return result;
}

void PathSettings::setConfigurablePath(const QString &key, const QString& path)
{
  if (path.isEmpty()) {
    remove(m_Settings, "Settings", key);
  } else {
    set(m_Settings, "Settings", key, path);
  }
}

QString PathSettings::base() const
{
  return QDir::fromNativeSeparators(get<QString>(m_Settings,
    "Settings", "base_directory", qApp->property("dataPath").toString()));
}

QString PathSettings::downloads(bool resolve) const
{
  return getConfigurablePath(
    "download_directory",
    ToQString(AppConfig::downloadPath()),
    resolve);
}

QString PathSettings::cache(bool resolve) const
{
  return getConfigurablePath(
    "cache_directory",
    ToQString(AppConfig::cachePath()),
    resolve);
}

QString PathSettings::mods(bool resolve) const
{
  return getConfigurablePath(
    "mod_directory",
    ToQString(AppConfig::modsPath()),
    resolve);
}

QString PathSettings::profiles(bool resolve) const
{
  return getConfigurablePath(
    "profiles_directory",
    ToQString(AppConfig::profilesPath()),
    resolve);
}

QString PathSettings::overwrite(bool resolve) const
{
  return getConfigurablePath(
    "overwrite_directory",
    ToQString(AppConfig::overwritePath()),
    resolve);
}

void PathSettings::setBase(const QString& path)
{
  if (path.isEmpty()) {
    remove(m_Settings, "Settings", "base_directory");
  } else {
    set(m_Settings, "Settings", "base_directory", path);
  }
}

void PathSettings::setDownloads(const QString& path)
{
  setConfigurablePath("download_directory", path);
}

void PathSettings::setMods(const QString& path)
{
  setConfigurablePath("mod_directory", path);
}

void PathSettings::setCache(const QString& path)
{
  setConfigurablePath("cache_directory", path);
}

void PathSettings::setProfiles(const QString& path)
{
  setConfigurablePath("profiles_directory", path);
}

void PathSettings::setOverwrite(const QString& path)
{
  setConfigurablePath("overwrite_directory", path);
}


NetworkSettings::NetworkSettings(QSettings& settings)
  : m_Settings(settings)
{
}

bool NetworkSettings::offlineMode() const
{
  return get<bool>(m_Settings, "Settings", "offline_mode", false);
}

void NetworkSettings::setOfflineMode(bool b)
{
  set(m_Settings, "Settings", "offline_mode", b);
}

bool NetworkSettings::useProxy() const
{
  return get<bool>(m_Settings, "Settings", "use_proxy", false);
}

void NetworkSettings::setUseProxy(bool b)
{
  set(m_Settings, "Settings", "use_proxy", b);
}

void NetworkSettings::setDownloadSpeed(const QString& name, int bytesPerSecond)
{
  auto current = servers();

  for (auto& server : current) {
    if (server.name() == name) {
      server.addDownload(bytesPerSecond);
      updateServers(current);
      return;
    }
  }

  log::error(
    "server '{}' not found while trying to add a download with bps {}",
    name, bytesPerSecond);
}

ServerList NetworkSettings::servers() const
{
  ServerList list;

  {
    ScopedReadArray sra(m_Settings, "Servers");

    sra.for_each([&] {
      ServerInfo::SpeedList lastDownloads;

      const auto lastDownloadsString = sra.get<QString>("lastDownloads", "");

      for (const auto& s : lastDownloadsString.split(" ")) {
        const auto bytesPerSecond = s.toInt();
        if (bytesPerSecond > 0) {
          lastDownloads.push_back(bytesPerSecond);
        }
      }

      ServerInfo server(
        sra.get<QString>("name", ""),
        sra.get<bool>("premium", false),
        QDate::fromString(sra.get<QString>("lastSeen", ""), Qt::ISODate),
        sra.get<int>("preferred", 0),
        lastDownloads);

      list.add(std::move(server));
      });
  }

  return list;
}

void NetworkSettings::updateServers(ServerList newServers)
{
  // clean up unavailable servers
  newServers.cleanup();

  const auto current = servers();

  if (current.size() > newServers.size()) {
    // Qt can't remove array elements, the section must be cleared
    removeSection(m_Settings, "Servers");
  }


  ScopedWriteArray swa(m_Settings, "Servers", newServers.size());

  for (const auto& server : newServers) {
    swa.next();

    swa.set("name", server.name());
    swa.set("premium", server.isPremium());
    swa.set("lastSeen", server.lastSeen().toString(Qt::ISODate));
    swa.set("preferred", server.preferred());

    QString lastDownloads;
    for (const auto& speed : server.lastDownloads()) {
      if (speed > 0) {
        lastDownloads += QString("%1 ").arg(speed);
      }
    }

    swa.set("lastDownloads", lastDownloads.trimmed());
  }
}

void NetworkSettings::updateFromOldMap()
{
  // servers used to be a map of byte arrays until 2.2.1, it's now an array of
  // individual values instead
  //
  // so post 2.2.1, only one key is returned: "size", the size of the arrays;
  // in 2.2.1, one key per server is returned

  // sanity check that this is really 2.2.1
  {
    const QStringList keys = ScopedGroup(m_Settings, "Servers").keys();

    for (auto&& k : keys) {
      if (k == "size") {
        // this looks like an array, so the upgrade was probably already done
        return;
      }
    }
  }

  const auto servers = serversFromOldMap();
  removeSection(m_Settings, "Servers");
  updateServers(servers);
}

ServerList NetworkSettings::serversFromOldMap() const
{
  // for 2.2.1 and before

  ServerList list;
  const ScopedGroup sg(m_Settings, "Servers");

  sg.for_each([&](auto&& serverKey) {
    QVariantMap data = sg.get<QVariantMap>(serverKey);

    ServerInfo server(
      serverKey,
      data["premium"].toBool(),
      data["lastSeen"].toDate(),
      data["preferred"].toInt(),
      {});

    // ignoring download count and speed, it's now a list of values instead of
    // a total

    list.add(std::move(server));
  });

  return list;
}

void NetworkSettings::dump() const
{
  log::debug("servers:");

  for (const auto& server : servers()) {
    QString lastDownloads;
    for (auto speed : server.lastDownloads()) {
      lastDownloads += QString("%1 ").arg(speed);
    }

    log::debug(
      "  . {} premium={} lastSeen={} preferred={} lastDownloads={}",
      server.name(),
      server.isPremium() ? "yes" : "no",
      server.lastSeen().toString(Qt::ISODate),
      server.preferred(),
      lastDownloads.trimmed());
  }
}


NexusSettings::NexusSettings(Settings& parent, QSettings& settings)
  : m_Parent(parent), m_Settings(settings)
{
}

bool NexusSettings::apiKey(QString& apiKey) const
{
  QString tempKey = getWindowsCredential("APIKEY");
  if (tempKey.isEmpty())
    return false;

  apiKey = tempKey;
  return true;
}

bool NexusSettings::setApiKey(const QString& apiKey)
{
  if (!setWindowsCredential("APIKEY", apiKey)) {
    const auto e = GetLastError();
    log::error("Storing API key failed: {}", formatSystemMessage(e));
    return false;
  }

  return true;
}

bool NexusSettings::clearApiKey()
{
  return setApiKey("");
}

bool NexusSettings::hasApiKey() const
{
  return !getWindowsCredential("APIKEY").isEmpty();
}

bool NexusSettings::endorsementIntegration() const
{
  return get<bool>(m_Settings, "Settings", "endorsement_integration", true);
}

void NexusSettings::setEndorsementIntegration(bool b) const
{
  set(m_Settings, "Settings", "endorsement_integration", b);
}

EndorsementState NexusSettings::endorsementState() const
{
  return endorsementStateFromString(
    get<QString>(m_Settings, "General", "endorse_state", ""));
}

void NexusSettings::setEndorsementState(EndorsementState s)
{
  const auto v = toString(s);

  if (v.isEmpty()) {
    remove(m_Settings, "General", "endorse_state");
  } else {
    set(m_Settings, "General", "endorse_state", v);
  }
}

void NexusSettings::registerAsNXMHandler(bool force)
{
  const auto nxmPath = QCoreApplication::applicationDirPath() + "/nxmhandler.exe";
  const auto executable = QCoreApplication::applicationFilePath();

  QString mode = force ? "forcereg" : "reg";
  QString parameters = mode + " " + m_Parent.game().plugin()->gameShortName();
  for (const QString& altGame : m_Parent.game().plugin()->validShortNames()) {
    parameters += "," + altGame;
  }
  parameters += " \"" + executable + "\"";

  const auto r = shell::Execute(nxmPath, parameters);

  if (!r.success()) {
    QMessageBox::critical(
      nullptr, QObject::tr("Failed"),
      QObject::tr("Failed to start the helper application: %1").arg(r.toString()));
  }
}


SteamSettings::SteamSettings(Settings& parent, QSettings& settings)
  : m_Parent(parent), m_Settings(settings)
{
}

QString SteamSettings::appID() const
{
  return get<QString>(
    m_Settings, "Settings", "app_id", m_Parent.game().plugin()->steamAPPId());
}

void SteamSettings::setAppID(const QString& id)
{
  if (id.isEmpty()) {
    remove(m_Settings, "Settings", "app_id");
  } else {
    set(m_Settings, "Settings", "app_id", id);
  }
}

bool SteamSettings::login(QString &username, QString &password) const
{
  username = get<QString>(m_Settings, "Settings", "steam_username", "");
  password = getWindowsCredential("steam_password");

  return !username.isEmpty() && !password.isEmpty();
}

void SteamSettings::setLogin(QString username, QString password)
{
  if (username == "") {
    remove(m_Settings, "Settings", "steam_username");
    password = "";
  } else {
    set(m_Settings, "Settings", "steam_username", username);
  }

  if (!setWindowsCredential("steam_password", password)) {
    const auto e = GetLastError();
    log::error("Storing or deleting password failed: {}", formatSystemMessage(e));
  }
}


InterfaceSettings::InterfaceSettings(QSettings& settings)
  : m_Settings(settings)
{
}

bool InterfaceSettings::lockGUI() const
{
  return get<bool>(m_Settings, "Settings", "lock_gui", true);
}

void InterfaceSettings::setLockGUI(bool b)
{
  set(m_Settings, "Settings", "lock_gui", b);
}

std::optional<QString> InterfaceSettings::styleName() const
{
  return getOptional<QString>(m_Settings, "Settings", "style");
}

void InterfaceSettings::setStyleName(const QString& name)
{
  set(m_Settings, "Settings", "style", name);
}

bool InterfaceSettings::compactDownloads() const
{
  return get<bool>(m_Settings, "Settings", "compact_downloads", false);
}

void InterfaceSettings::setCompactDownloads(bool b)
{
  set(m_Settings, "Settings", "compact_downloads", b);
}

bool InterfaceSettings::metaDownloads() const
{
  return get<bool>(m_Settings, "Settings", "meta_downloads", false);
}

void InterfaceSettings::setMetaDownloads(bool b)
{
  set(m_Settings, "Settings", "meta_downloads", b);
}

bool InterfaceSettings::hideAPICounter() const
{
  return get<bool>(m_Settings, "Settings", "hide_api_counter", false);
}

void InterfaceSettings::setHideAPICounter(bool b)
{
  set(m_Settings, "Settings", "hide_api_counter", b);
}

bool InterfaceSettings::displayForeign() const
{
  return get<bool>(m_Settings, "Settings", "display_foreign", true);
}

void InterfaceSettings::setDisplayForeign(bool b)
{
  set(m_Settings, "Settings", "display_foreign", b);
}

QString InterfaceSettings::language()
{
  QString result = get<QString>(m_Settings, "Settings", "language", "");

  if (result.isEmpty()) {
    QStringList languagePreferences = QLocale::system().uiLanguages();

    if (languagePreferences.length() > 0) {
      // the users most favoritest language
      result = languagePreferences.at(0);
    } else {
      // fallback system locale
      result = QLocale::system().name();
    }
  }

  return result;
}

void InterfaceSettings::setLanguage(const QString& name)
{
  set(m_Settings, "Settings", "language", name);
}

bool InterfaceSettings::isTutorialCompleted(const QString& windowName) const
{
  return get<bool>(m_Settings, "CompletedWindowTutorials", windowName, false);
}

void InterfaceSettings::setTutorialCompleted(const QString& windowName, bool b)
{
  set(m_Settings, "CompletedWindowTutorials", windowName, b);
}


DiagnosticsSettings::DiagnosticsSettings(QSettings& settings)
  : m_Settings(settings)
{
}

log::Levels DiagnosticsSettings::logLevel() const
{
  return get<log::Levels>(m_Settings, "Settings", "log_level", log::Levels::Info);
}

void DiagnosticsSettings::setLogLevel(log::Levels level)
{
  set(m_Settings, "Settings", "log_level", level);
}

CrashDumpsType DiagnosticsSettings::crashDumpsType() const
{
  return get<CrashDumpsType>(m_Settings,
    "Settings", "crash_dumps_type", CrashDumpsType::Mini);
}

void DiagnosticsSettings::setCrashDumpsType(CrashDumpsType type)
{
  set(m_Settings, "Settings", "crash_dumps_type", type);
}

int DiagnosticsSettings::crashDumpsMax() const
{
  return get<int>(m_Settings, "Settings", "crash_dumps_max", 5);
}

void DiagnosticsSettings::setCrashDumpsMax(int n)
{
  set(m_Settings, "Settings", "crash_dumps_max", n);
}

std::chrono::seconds DiagnosticsSettings::spawnDelay() const
{
  return std::chrono::seconds(
    get<int>(m_Settings, "Settings", "spawn_delay", 0));
}

void DiagnosticsSettings::setSpawnDelay(std::chrono::seconds t)
{
  set(m_Settings, "Settings", "spawn_delay", t.count());
}
