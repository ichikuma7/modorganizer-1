/*
Copyright (C) 2015 Sebastian Herbord. All rights reserved.

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

#include "usvfsconnector.h"
#include "settings.h"
#include "organizercore.h"
#include "envmodule.h"
#include "shared/util.h"
#include <memory>
#include <sstream>
#include <iomanip>
#include <usvfs.h>
#include <QTemporaryFile>
#include <QProgressDialog>
#include <QDateTime>
#include <QCoreApplication>
#include <qstandardpaths.h>

static const char SHMID[] = "mod_organizer_instance";
using namespace MOBase;

std::string to_hex(void *bufferIn, size_t bufferSize)
{
  unsigned char *buffer = static_cast<unsigned char *>(bufferIn);
  std::ostringstream temp;
  temp << std::hex;
  for (size_t i = 0; i < bufferSize; ++i) {
    temp << std::setfill('0') << std::setw(2) << (unsigned int)buffer[i];
    if ((i % 16) == 15) {
      temp << "\n";
    } else {
      temp << " ";
    }
  }
  return temp.str();
}


LogWorker::LogWorker()
  : m_Buffer(1024, '\0')
  , m_QuitRequested(false)
  , m_LogFile(qApp->property("dataPath").toString()
              + QString("/logs/usvfs-%1.log")
                    .arg(QDateTime::currentDateTimeUtc().toString(
                        "yyyy-MM-dd_hh-mm-ss")))
{
  m_LogFile.open(QIODevice::WriteOnly);
  log::debug("usvfs log messages are written to {}", m_LogFile.fileName());
}

LogWorker::~LogWorker()
{
}

void LogWorker::process()
{
  int noLogCycles = 0;
  while (!m_QuitRequested) {
    if (GetLogMessages(&m_Buffer[0], m_Buffer.size(), false)) {
      m_LogFile.write(m_Buffer.c_str());
      m_LogFile.write("\n");
      m_LogFile.flush();
      noLogCycles = 0;
    } else {
      QThread::msleep(std::min(40, noLogCycles) * 5);
      ++noLogCycles;
    }
  }
  emit finished();
}

void LogWorker::exit()
{
  m_QuitRequested = true;
}

LogLevel toUsvfsLogLevel(log::Levels level)
{
  switch (level) {
    case log::Info:
      return LogLevel::Info;
    case log::Warning:
      return LogLevel::Warning;
    case log::Error:
      return LogLevel::Error;
    case log::Debug:  // fall-through
    default:
      return LogLevel::Debug;
  }
}

CrashDumpsType crashDumpsType(int type)
{
  switch (static_cast<CrashDumpsType>(type)) {
  case CrashDumpsType::Mini:
    return CrashDumpsType::Mini;
  case CrashDumpsType::Data:
    return CrashDumpsType::Data;
  case CrashDumpsType::Full:
    return CrashDumpsType::Full;
  default:
    return CrashDumpsType::None;
  }
}

UsvfsConnector::UsvfsConnector()
{
  using namespace std::chrono;

  const auto& s = Settings::instance();

  const LogLevel logLevel = toUsvfsLogLevel(s.diagnostics().logLevel());
  const CrashDumpsType dumpType = s.diagnostics().crashDumpsType();
  const auto delay = duration_cast<milliseconds>(s.diagnostics().spawnDelay());
  std::string dumpPath = MOShared::ToString(OrganizerCore::crashDumpsPath(), true);

  usvfsParameters* params = usvfsCreateParameters();

  usvfsSetInstanceName(params, SHMID);
  usvfsSetDebugMode(params, false);
  usvfsSetLogLevel(params, logLevel);
  usvfsSetCrashDumpType(params, dumpType);
  usvfsSetCrashDumpPath(params, dumpPath.c_str());
  usvfsSetProcessDelay(params, delay.count());

  InitLogging(false);

  log::debug(
    "initializing usvfs:\n"
    " . instance: {}\n"
    " . log: {}\n"
    " . dump: {} ({})",
    SHMID,
    usvfsLogLevelToString(logLevel),
    dumpPath.c_str(),
    usvfsCrashDumpTypeToString(dumpType));

  usvfsCreateVFS(params);
  usvfsFreeParameters(params);

  ClearExecutableBlacklist();
  for (auto exec : s.executablesBlacklist().split(";")) {
    std::wstring buf = exec.toStdWString();
    BlacklistExecutable(buf.data());
  }

  ClearLibraryForceLoads();

  m_LogWorker.moveToThread(&m_WorkerThread);

  connect(&m_WorkerThread, SIGNAL(started()), &m_LogWorker, SLOT(process()));
  connect(&m_LogWorker, SIGNAL(finished()), &m_WorkerThread, SLOT(quit()));

  m_WorkerThread.start(QThread::LowestPriority);
}

UsvfsConnector::~UsvfsConnector()
{
  DisconnectVFS();
  m_LogWorker.exit();
  m_WorkerThread.quit();
  m_WorkerThread.wait();
}


void UsvfsConnector::updateMapping(const MappingType &mapping)
{
  QProgressDialog progress;
  progress.setLabelText(tr("Preparing vfs"));
  progress.setMaximum(static_cast<int>(mapping.size()));
  progress.show();
  int value = 0;
  int files = 0;
  int dirs = 0;

  log::debug("Updating VFS mappings...");

  ClearVirtualMappings();

  for (auto map : mapping) {
    if (progress.wasCanceled()) {
      ClearVirtualMappings();
      throw UsvfsConnectorException("VFS mapping canceled by user");
    }
    progress.setValue(value++);
    if (value % 10 == 0) {
      QCoreApplication::processEvents();
    }

    if (map.isDirectory) {
      VirtualLinkDirectoryStatic(map.source.toStdWString().c_str(),
                                 map.destination.toStdWString().c_str(),
                                 (map.createTarget ? LINKFLAG_CREATETARGET : 0)
                                     | LINKFLAG_RECURSIVE
                                 );
      ++dirs;
    } else {
      VirtualLinkFile(map.source.toStdWString().c_str(),
                      map.destination.toStdWString().c_str(), 0);
      ++files;
    }
  }

  log::debug("VFS mappings updated <linked {} dirs, {} files>", dirs, files);
}

void UsvfsConnector::updateParams(
  MOBase::log::Levels logLevel, CrashDumpsType crashDumpsType,
  const QString& crashDumpsPath, std::chrono::seconds spawnDelay,
  QString executableBlacklist)
{
  using namespace std::chrono;

  usvfsParameters* p = usvfsCreateParameters();

  usvfsSetDebugMode(p, FALSE);
  usvfsSetLogLevel(p, toUsvfsLogLevel(logLevel));
  usvfsSetCrashDumpType(p, crashDumpsType);
  usvfsSetCrashDumpPath(p, crashDumpsPath.toStdString().c_str());
  usvfsSetProcessDelay(p, duration_cast<milliseconds>(spawnDelay).count());

  usvfsUpdateParameters(p);
  usvfsFreeParameters(p);

  ClearExecutableBlacklist();
  for (auto exec : executableBlacklist.split(";")) {
    std::wstring buf = exec.toStdWString();
    BlacklistExecutable(buf.data());
  }
}

void UsvfsConnector::updateForcedLibraries(const QList<MOBase::ExecutableForcedLoadSetting> &forcedLibraries)
{
  ClearLibraryForceLoads();
  for (auto setting : forcedLibraries) {
    if (setting.enabled()) {
      ForceLoadLibrary(
        setting.process().toStdWString().data(),
        setting.library().toStdWString().data()
      );
    }
  }
}


std::vector<HANDLE> getRunningUSVFSProcesses()
{
  std::vector<DWORD> pids;

  {
    size_t count = 0;
    DWORD* buffer = nullptr;
    if (!::GetVFSProcessList2(&count, &buffer)) {
      log::error("failed to get usvfs process list");
      return {};
    }

    if (buffer) {
      pids.assign(buffer, buffer + count);
      std::free(buffer);
    }
  }

  const auto thisPid = GetCurrentProcessId();
  std::vector<HANDLE> v;

  for (auto&& pid : pids) {
    if (pid == thisPid) {
      continue; // obviously don't wait for MO process
    }

    HANDLE handle = ::OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);

    if (handle == INVALID_HANDLE_VALUE) {
      const auto e = GetLastError();

      log::warn(
        "failed to open usvfs process {}: {}",
        pid, formatSystemMessage(e));

      continue;
    }

    v.push_back(handle);
  }

  return v;
}
