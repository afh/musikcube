//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2004-2021 musikcube team
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the author nor the names of other contributors may
//      be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"

#include <musikcore/debug.h>
#include <musikcore/library/Indexer.h>

#include <musikcore/config.h>
#include <musikcore/debug.h>
#include <musikcore/library/track/IndexerTrack.h>
#include <musikcore/library/track/LibraryTrack.h>
#include <musikcore/library/query/TrackMetadataQuery.h>
#include <musikcore/library/LocalLibraryConstants.h>
#include <musikcore/library/LibraryFactory.h>
#include <musikcore/db/Connection.h>
#include <musikcore/db/Statement.h>
#include <musikcore/plugin/PluginFactory.h>
#include <musikcore/support/Common.h>
#include <musikcore/support/Preferences.h>
#include <musikcore/support/PreferenceKeys.h>
#include <musikcore/sdk/IAnalyzer.h>
#include <musikcore/sdk/IIndexerSource.h>
#include <musikcore/audio/Stream.h>
#include <musikcore/support/ThreadGroup.h>

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <functional>

#define STRESS_TEST_DB 0

constexpr const char* TAG = "Indexer";
constexpr size_t TRANSACTION_INTERVAL = 300;
static FILE* logFile = nullptr;

#ifdef __arm__
constexpr int DEFAULT_MAX_THREADS = 2;
#else
constexpr int DEFAULT_MAX_THREADS = 4;
#endif

namespace std {
    namespace fs = std::filesystem;
}

using namespace musik::core;
using namespace musik::core::sdk;
using namespace musik::core::audio;
using namespace musik::core::library;
using namespace musik::core::db;
using namespace musik::core::library::query;

using Thread = std::unique_ptr<std::thread>;

using TagReaderDestroyer = PluginFactory::ReleaseDeleter<ITagReader>;
using DecoderDeleter = PluginFactory::ReleaseDeleter<IDecoderFactory>;
using SourceDeleter = PluginFactory::ReleaseDeleter<IIndexerSource>;

static void openLogFile() {
    if (!logFile) {
        std::string path = GetDataDirectory() + "/indexer_log.txt";
#ifdef WIN32
        logFile = _wfopen(u8to16(path).c_str(), L"w");
#else
        logFile = fopen(path.c_str(), "w");
#endif
    }
}

static void closeLogFile() noexcept {
    if (logFile) {
        fclose(logFile);
        logFile = nullptr;
    }
}

static std::string normalizePath(const std::string& path) {
    return std::fs::path(std::fs::u8path(path)).make_preferred().u8string();
}

Indexer::Indexer(const std::string& libraryPath, const std::string& dbFilename)
: thread(nullptr)
, incrementalUrisScanned(0)
, totalUrisScanned(0)
, state(StateStopped)
, prefs(Preferences::ForComponent(prefs::components::Settings)) {
    if (prefs->GetBool(prefs::keys::IndexerLogEnabled, false) && !logFile) {
        openLogFile();
    }

    this->tagReaders = PluginFactory::Instance()
        .QueryInterface<ITagReader, TagReaderDestroyer>("GetTagReader");

    this->audioDecoders = PluginFactory::Instance()
        .QueryInterface<IDecoderFactory, DecoderDeleter>("GetDecoderFactory");

    this->sources = PluginFactory::Instance()
        .QueryInterface<IIndexerSource, SourceDeleter>("GetIndexerSource");

    this->dbFilename = dbFilename;
    this->libraryPath = libraryPath;

    db::Connection connection;
    connection.Open(this->dbFilename.c_str());
    db::Statement stmt("SELECT path FROM paths ORDER BY id", connection);
    while (stmt.Step() == db::Row) {
        this->paths.push_back(stmt.ColumnText(0));
    }
}

Indexer::~Indexer() {
    closeLogFile();
    this->Shutdown();
}

void Indexer::Shutdown() {
    if (this->thread) {
        {
            std::unique_lock<decltype(this->stateMutex)> lock(this->stateMutex);

            this->syncQueue.clear();
            this->state = StateStopping;

            if (this->currentSource) {
                this->currentSource->Interrupt();
            }
        }

        this->waitCondition.notify_all();
        this->thread->join();
        this->thread.reset();
    }
}

void Indexer::Schedule(SyncType type) {
    this->Schedule(type, nullptr);
}

void Indexer::Schedule(SyncType type, IIndexerSource* source) {
    std::unique_lock<decltype(this->stateMutex)> lock(this->stateMutex);

    if (!this->thread) {
        this->state = StateIdle;
        this->thread = std::make_unique<std::thread>(std::bind(&Indexer::ThreadLoop, this));
    }

    const int sourceId = source ? source->SourceId() : 0;
    for (const SyncContext& context : this->syncQueue) {
        if (context.type == type && context.sourceId == sourceId) {
            return;
        }
    }

    SyncContext context;
    context.type = type;
    context.sourceId = sourceId;
    syncQueue.push_back(context);

    this->waitCondition.notify_all();
}

void Indexer::AddPath(const std::string& path) {
    Indexer::AddRemoveContext context;
    context.add = true;
    context.path = NormalizeDir(path);

    {
        std::unique_lock<decltype(this->stateMutex)> lock(this->stateMutex);

        if (std::find(this->paths.begin(), this->paths.end(), path) == this->paths.end()) {
            this->paths.push_back(path);
        }

        this->addRemoveQueue.push_back(context);
    }
}

void Indexer::RemovePath(const std::string& path) {
    Indexer::AddRemoveContext context;
    context.add = false;
    context.path = NormalizeDir(path);

    {
        std::unique_lock<decltype(this->stateMutex)> lock(this->stateMutex);

        auto it = std::find(this->paths.begin(), this->paths.end(), path);
        if (it != this->paths.end()) {
            this->paths.erase(it);
        }

        this->addRemoveQueue.push_back(context);
    }
}

void Indexer::Synchronize(const SyncContext& context, asio::io_service* io) {
    LocalLibrary::CreateIndexes(this->dbConnection);

    IndexerTrack::OnIndexerStarted(this->dbConnection);

    this->ProcessAddRemoveQueue();

    this->incrementalUrisScanned = 0;
    this->totalUrisScanned = 0;

    /* always remove tracks that no longer have a corresponding source */
    for (const auto id : this->GetOrphanedSourceIds()) {
        this->RemoveAllForSourceId(id);
    }

    auto type = context.type;
    const auto sourceId = context.sourceId;
    if (type == SyncType::Rebuild) {
        LocalLibrary::InvalidateTrackMetadata(this->dbConnection);

        /* for sources with stable ids: just nuke all of the records and allow
        a rebuild from scratch; things like playlists will remain intact.
        this ensures tracks that should be removed, are */
        for (auto source: sources) {
            if (source->HasStableIds()) {
                this->RemoveAll(source.get());
            }
        }

        type = SyncType::All;
    }

    std::vector<std::string> paths;
    std::vector<int64_t> pathIds;

    /* resolve all the path and path ids (required for local files */
    db::Statement stmt("SELECT id, path FROM paths", this->dbConnection);

    while (stmt.Step() == db::Row) {
        try {
            const int64_t id = stmt.ColumnInt64(0);
            std::string path = stmt.ColumnText(1);
            std::fs::path dir(std::fs::u8path(path));

            if (std::fs::exists(dir)) {
                paths.push_back(path);
                pathIds.push_back(id);
            }
        }
        catch(...) {
        }
    }

    /* refresh sources */
    for (auto it : this->sources) {
        if (this->Bail()) {
            break;
        }

        if (sourceId != 0 && sourceId != it->SourceId()) {
            continue; /* asked to scan a specific source, and this isn't it. */
        }

        this->currentSource = it;
        if (this->SyncSource(it.get(), paths) == ScanRollback) {
            this->trackTransaction->Cancel();
        }
        this->trackTransaction->CommitAndRestart();

        if (sourceId != 0) {
            break; /* done with the one we were asked to scan */
        }
    }

    this->currentSource.reset();

    /* process local files */
    if (type != SyncType::Sources) {
        if (logFile) {
            fprintf(logFile, "\n\nSYNCING LOCAL FILES:\n");
        }

        /* read metadata from the files  */
        for (std::size_t i = 0; i < paths.size(); ++i) {
            musik::debug::info(TAG, "scanning " + paths[i]);
            this->SyncDirectory(io, paths[i], paths[i], pathIds[i]);
        }

        /* close any pending transaction */
        this->trackTransaction->CommitAndRestart();

        /* re-index */
        LocalLibrary::CreateIndexes(this->dbConnection);
    }
}

void Indexer::FinalizeSync(const SyncContext& context) {
    /* remove undesired entries from db (files themselves will remain) */
    musik::debug::info(TAG, "cleanup 1/2");

    const auto type = context.type;

    if (type != SyncType::Sources) {
        if (!this->Bail()) {
            this->SyncDelete();
        }
    }

    /* cleanup -- remove stale artists, albums, genres, etc */
    musik::debug::info(TAG, "cleanup 2/2");

    if (!this->Bail()) {
        this->SyncCleanup();
    }

    /* optimize and sort */
    musik::debug::info(TAG, "optimizing");

    if (!this->Bail()) {
        this->SyncOptimize();
    }

    /* run analyzers. */
    this->RunAnalyzers();

    IndexerTrack::OnIndexerFinished(this->dbConnection);
}

void Indexer::ReadMetadataFromFile(
    asio::io_service* io,
    const std::fs::path& file,
    const std::string& pathId)
{
    /* we do this here because work may have already been queued before the abort
    flag was raised */
    if (io && this->Bail()) {
        if (!io->stopped()) {
            musik::debug::info(TAG, "run aborted");
            io->stop();
        }
        return;
    }

    #define APPEND_LOG(x) if (logFile) { fprintf(logFile, "    - [%s] %s\n", x, file.u8string().c_str()); }

    musik::core::IndexerTrack track(0);

    const bool needsToBeIndexed = track.NeedsToBeIndexed(file, this->dbConnection);

    /* get cached filesize, parts, size, etc */
    if (needsToBeIndexed) {
        APPEND_LOG("needs to be indexed")

        bool saveToDb = false;

        /* read the tag from the plugin */
        TagStore store(track);
        typedef TagReaderList::iterator Iterator;
        Iterator it = this->tagReaders.begin();
        while (it != this->tagReaders.end()) {
            try {
                if ((*it)->CanRead(track.GetString("extension").c_str())) {
                    APPEND_LOG("can read")
                    if ((*it)->Read(file.u8string().c_str(), &store)) {
                        APPEND_LOG("did read")
                        saveToDb = true;
                        break;
                    }
                }
            }
            catch (...) {
                /* sometimes people have files with crazy tags that cause the
                tag reader to throw fits. not a lot we can do. just move on. */
            }

            it++;
        }

        /* write it to the db, if read successfully */
        if (saveToDb) {
            track.SetValue("path_id", pathId.c_str());
            track.Save(this->dbConnection, this->libraryPath);

#if STRESS_TEST_DB != 0
            #define INC(track, key, x) \
                { \
                    std::string val = track.GetValue(key); \
                    val += (char) ('a' + x); \
                    track.ClearValue(key); \
                    track.SetValue(key, val.c_str()); \
                }

            for (int i = 0; i < 20; i++) {
                track.SetId(0);
                INC(track, "title", i);
                INC(track, "artist", i);
                INC(track, "album_artist", i);
                INC(track, "album", i);
                track.Save(this->dbConnection, this->libraryPath);
            }
#endif
        }
        else {
            APPEND_LOG("read failed")
        }
    }
    else {
        APPEND_LOG("does not need to be indexed")
    }

    #undef APPEND_LOG

    this->IncrementTracksScanned();
}

inline void Indexer::IncrementTracksScanned(int delta) {
    std::unique_lock<std::mutex> lock(IndexerTrack::sharedWriteMutex);

    this->incrementalUrisScanned.fetch_add(delta);
    this->totalUrisScanned.fetch_add(delta);

    const int interval = prefs->GetInt(
        prefs::keys::IndexerTransactionInterval, TRANSACTION_INTERVAL);

    if (this->incrementalUrisScanned > TRANSACTION_INTERVAL) {
        this->trackTransaction->CommitAndRestart();
        this->Progress(this->totalUrisScanned);
        this->incrementalUrisScanned = 0;
    }
}

void Indexer::SyncDirectory(
    asio::io_service* io,
    const std::string &syncRoot,
    const std::string &currentPath,
    int64_t pathId)
{
    std::string normalizedSyncRoot = NormalizeDir(syncRoot);
    std::string normalizedCurrentPath = NormalizeDir(currentPath);

    /* start recursive filesystem scan */

    try {
        /* for each file in the current path... */
        std::fs::path path(std::fs::u8path(currentPath));
        std::fs::directory_iterator end;
        std::fs::directory_iterator file(path);

        std::string pathIdStr = std::to_string(pathId);
        std::vector<Thread> threads;

        for( ; file != end && !this->Bail(); file++) {
            if (this->Bail()) {
                break;
            }
            if (is_directory(file->status())) {
                /* recursion here */
                this->SyncDirectory(io, syncRoot, file->path().u8string(), pathId);
            }
            else {
                try {
                    std::string extension = file->path().extension().u8string();
                    for (auto it : this->tagReaders) {
                        if (it->CanRead(extension.c_str())) {
                            if (io) {
                                io->post(std::bind(
                                    &Indexer::ReadMetadataFromFile,
                                    this,
                                    io,
                                    file->path(),
                                    pathIdStr));
                            }
                            else {
                                this->ReadMetadataFromFile(nullptr, file->path(), pathIdStr);
                            }
                            break;
                        }
                    }
                }
                catch (...) {
                    /* std::filesystem may throw trying to stat the file */
                }
            }
        }
    }
    catch(...) {
        /* std::filesystem may throw trying to open the directory */
    }
}

ScanResult Indexer::SyncSource(
    IIndexerSource* source,
    const std::vector<std::string>& paths)
{
    debug::info(TAG, u8fmt("indexer source %d running...", source->SourceId()));

    if (source->SourceId() == 0) {
        return ScanRollback;
    }

    /* only commit if explicitly succeeded */
    ScanResult result = ScanRollback;

    source->OnBeforeScan();

    try {
        /* alloc/init fun interop; we pass all paths to the indexer source */
        const char** pathsList = new const char*[paths.size()];
        for (size_t i = 0; i < paths.size(); i++) {
            auto& p = paths[i];
            auto sz = p.size();
            auto dst = new char[sz + 1];
            strncpy(dst, p.c_str(), sz);
            dst[sz] = '\0';
            pathsList[i] = dst;
        }

        /* now tell it to do a wide-open scan. it can use this opportunity to
        remove old tracks, or add new ones. */
        try {
            result = source->Scan(this, pathsList, (unsigned int) paths.size());
        }
        catch (...) {
            debug::error("Indexer", "failed to index " + std::to_string(source->SourceId()));
        }

        /* free fun interop -- it's done with the paths now. */
        for (size_t i = 0; i < paths.size(); i++) {
            delete[] pathsList[i];
        }
        delete[] pathsList;

        /* finally, allow the source to update metadata for any tracks that it
        previously indexed, if it needs to. */
        {
            if (!this->Bail() && source->NeedsTrackScan()) {
                db::Statement tracks(
                    "SELECT id, filename, external_id FROM tracks WHERE source_id=? ORDER BY id",
                    this->dbConnection);

                tracks.BindInt32(0, source->SourceId());
                while (tracks.Step() == db::Row) {
                    TrackPtr track = std::make_shared<IndexerTrack>(tracks.ColumnInt64(0));
                    track->SetValue(constants::Track::FILENAME, tracks.ColumnText(1));

                    if (logFile) {
                        fprintf(logFile, "    - %s\n", track->GetString(constants::Track::FILENAME).c_str());
                    }

                    TagStore* store = new TagStore(track);
                    source->ScanTrack(this, store, tracks.ColumnText(2));
                    store->Release();
                }
            }
        }

        debug::info(TAG, u8fmt("indexer source %d finished", source->SourceId()));
    }
    catch (...) {
        debug::error(TAG, u8fmt("indexer source %d crashed", source->SourceId()));
    }

    source->OnAfterScan();

    return result;
}

void Indexer::ThreadLoop() {
    std::fs::path thumbPath(std::fs::u8path(this->libraryPath + "thumbs/"));

    if (!std::fs::exists(thumbPath)) {
        std::fs::create_directories(thumbPath);
    }

    while (true) {
        /* wait for some work. */
        {
            std::unique_lock<decltype(this->stateMutex)> lock(this->stateMutex);
            while (!this->Bail() && this->syncQueue.size() == 0) {
                this->state = StateIdle;
                this->waitCondition.wait(lock);
            }
        }

        if (this->Bail()) {
            return;
        }

        const SyncContext context = this->syncQueue.front();
        this->syncQueue.pop_front();

        this->state = StateIndexing;
        this->Started();

        this->dbConnection.Open(this->dbFilename.c_str(), 0);
        this->trackTransaction = std::make_shared<db::ScopedTransaction>(this->dbConnection);

        const int threadCount = prefs->GetInt(
            prefs::keys::IndexerThreadCount, DEFAULT_MAX_THREADS);

        if (threadCount > 1) {
            asio::io_service io;
            asio::io_service::work work(io);
            ThreadGroup threadGroup;

            /* initialize the thread pool -- we'll use this to index tracks in parallel. */
            for (int i = 0; i < threadCount; i++) {
                threadGroup.create_thread([&io]() {
                    io.run();
                });
            }

            this->Synchronize(context, &io);

            /* done with sync, remove all the threads in the pool to free resources. they'll
            be re-created later if we index again. */
            io.post([&io]() {
                if (!io.stopped()) {
                    musik::debug::info(TAG, "scan completed successfully");
                    io.stop();
                }
            });

            threadGroup.join_all();
        }
        else {
            this->Synchronize(context, nullptr);
        }

        this->FinalizeSync(context);

        this->trackTransaction.reset();

        this->dbConnection.Close();

        if (!this->Bail()) {
            this->Progress(this->totalUrisScanned);
            this->Finished(this->totalUrisScanned);
        }

        musik::debug::info(TAG, "done!");
    }
}

void Indexer::SyncDelete() {
    /* remove all tracks that no longer reference a valid path entry */

    this->dbConnection.Execute("DELETE FROM tracks WHERE source_id == 0 AND path_id NOT IN (SELECT id FROM paths)");

    /* remove files that are no longer on the filesystem. */

    if (prefs->GetBool(prefs::keys::RemoveMissingFiles, true)) {
        db::Statement stmtRemove("DELETE FROM tracks WHERE id=?", this->dbConnection);

        db::Statement allTracks(
            "SELECT t.id, t.filename "
            "FROM tracks t "
            "WHERE source_id == 0", /* IIndexerSources delete their own tracks */
            this->dbConnection);

        while (allTracks.Step() == db::Row && !this->Bail()) {
            bool remove = false;
            std::string fn = allTracks.ColumnText(1);

            try {
                std::fs::path file(std::fs::u8path(fn));
                if (!std::fs::exists(file)) {
                    remove = true;
                }
            }
            catch (...) {
            }

            if (remove) {
                stmtRemove.BindInt32(0, allTracks.ColumnInt32(0));
                stmtRemove.Step();
                stmtRemove.Reset();
            }
        }
    }
}

void Indexer::SyncCleanup() {
    /* remove old artists */
    this->dbConnection.Execute("DELETE FROM track_artists WHERE track_id NOT IN (SELECT id FROM tracks)");
    this->dbConnection.Execute("DELETE FROM artists WHERE id NOT IN (SELECT DISTINCT(visual_artist_id) FROM tracks) AND id NOT IN (SELECT DISTINCT(album_artist_id) FROM tracks) AND id NOT IN (SELECT DISTINCT(artist_id) FROM track_artists)");

    /* remove old genres */
    this->dbConnection.Execute("DELETE FROM track_genres WHERE track_id NOT IN (SELECT id FROM tracks)");
    this->dbConnection.Execute("DELETE FROM genres WHERE id NOT IN (SELECT DISTINCT(visual_genre_id) FROM tracks) AND id NOT IN (SELECT DISTINCT(genre_id) FROM track_genres)");

    /* remove old albums */
    this->dbConnection.Execute("DELETE FROM albums WHERE id NOT IN (SELECT DISTINCT(album_id) FROM tracks)");

    /* orphaned metadata */
    this->dbConnection.Execute("DELETE FROM track_meta WHERE track_id NOT IN (SELECT id FROM tracks)");
    this->dbConnection.Execute("DELETE FROM meta_values WHERE id NOT IN (SELECT DISTINCT(meta_value_id) FROM track_meta)");
    this->dbConnection.Execute("DELETE FROM meta_keys WHERE id NOT IN (SELECT DISTINCT(meta_key_id) FROM meta_values)");

    /* orphaned replay gain and directories */
    this->dbConnection.Execute("DELETE FROM replay_gain WHERE track_id NOT IN (SELECT id FROM tracks)");
    this->dbConnection.Execute("DELETE FROM directories WHERE id NOT IN (SELECT DISTINCT directory_id FROM tracks)");

    /* NOTE: we used to remove orphaned local library tracks here, but we don't anymore because
    the indexer generates stable external ids by hashing various file and metadata fields */

    /* orphaned playlist tracks from source plugins that do not have stable
    ids need to be cleaned up. */
    for (auto source : this->sources) {
        if (!source->HasStableIds()) {
            std::string query =
                "DELETE FROM playlist_tracks "
                "WHERE source_id=? AND track_external_id NOT IN ( "
                "  SELECT DISTINCT external_id "
                "  FROM tracks "
                "  WHERE source_id == ?)";

            db::Statement stmt(query.c_str(), this->dbConnection);
            stmt.BindInt32(0, source->SourceId());
            stmt.BindInt32(1, source->SourceId());
            stmt.Step();
        }
    }

    this->SyncPlaylistTracksOrder();

    /* optimize and shrink */
    this->dbConnection.Execute("VACUUM");
}

void Indexer::SyncPlaylistTracksOrder() {
    /* make sure playlist sort orders are always sequential without holes. we
    do this anyway, as playlists are updated, but there's no way to guarantee
    it stays this way -- plugins, external processes, etc can cause problems */

    db::Statement playlists(
        "SELECT DISTINCT id FROM playlists",
        this->dbConnection);

    db::Statement tracks(
        "SELECT track_external_id, sort_order "
        "FROM playlist_tracks WHERE playlist_id=? "
        "ORDER BY sort_order",
        this->dbConnection);

    db::Statement update(
        "UPDATE playlist_tracks "
        "SET sort_order=? "
        "WHERE track_external_id=? AND sort_order=?",
        this->dbConnection);

    struct Record { std::string id; int order; };

    while (playlists.Step() == db::Row) {
        tracks.ResetAndUnbind();
        tracks.BindInt64(0, playlists.ColumnInt64(0));

        /* gotta cache these in memory because we can't update the
        table at the same time we're iterating */
        std::vector<Record> records;
        while (tracks.Step() == db::Row) {
            records.push_back({ tracks.ColumnText(0), tracks.ColumnInt32(1) });
        }

        int order = 0;
        for (auto& r : records) {
            update.ResetAndUnbind();
            update.BindInt32(0, order++);
            update.BindText(1, r.id);
            update.BindInt32(2, r.order);
            update.Step();
        }
    }
}

void Indexer::GetPaths(std::vector<std::string>& paths) {
    std::unique_lock<decltype(this->stateMutex)> lock(this->stateMutex);
    std::copy(this->paths.begin(), this->paths.end(), std::back_inserter(paths));
}

std::set<int> Indexer::GetOrphanedSourceIds() {
    /* build a list of source ids: `(x, y, z)` */
    std::string group = "(0"; /* 0 is the built-in source, it's always valid */
    for (size_t i = 0; i < this->sources.size(); i++) {
        group += "," + std::to_string(this->sources.at(i)->SourceId());
    }
    group += ")";

    std::string query =
        "SELECT DISTINCT source_id "
        "FROM tracks "
        "WHERE source_id NOT IN " + group;

    std::set<int> result;
    db::Statement stmt(query.c_str(), this->dbConnection);
    while (stmt.Step() == db::Row) {
        result.insert(stmt.ColumnInt32(0));
    }
    return result;
}

static int optimize(
    musik::core::db::Connection &connection,
    std::string singular,
    std::string plural)
{

    std::string outer = u8fmt(
        "SELECT id, lower(trim(name)) AS %s FROM %s ORDER BY %s",
        singular.c_str(), plural.c_str(), singular.c_str());

    db::Statement outerStmt(outer.c_str(), connection);

    std::string inner = u8fmt("UPDATE %s SET sort_order=? WHERE id=?", plural.c_str());
    db::Statement innerStmt(inner.c_str(), connection);

    int count = 0;
    while (outerStmt.Step() == db::Row) {
        innerStmt.BindInt32(0, count);
        innerStmt.BindInt64(1, outerStmt.ColumnInt64(0));
        innerStmt.Step();
        innerStmt.Reset();
        ++count;
    }

    std::this_thread::yield();

    return count;
}

void Indexer::SyncOptimize() {
    db::ScopedTransaction transaction(this->dbConnection);
    optimize(this->dbConnection, "genre", "genres");
    optimize(this->dbConnection, "artist", "artists");
    optimize(this->dbConnection, "album", "albums");
    optimize(this->dbConnection, "content", "meta_values");
}

void Indexer::ProcessAddRemoveQueue() {
    std::unique_lock<decltype(this->stateMutex)> lock(this->stateMutex);
    while (!this->addRemoveQueue.empty()) {
        AddRemoveContext context = this->addRemoveQueue.front();

        if (context.add) { /* insert new paths */
            db::Statement stmt("SELECT id FROM paths WHERE path=?", this->dbConnection);
            stmt.BindText(0, context.path);

            if (stmt.Step() != db::Row) {
                db::Statement insertPath("INSERT INTO paths (path) VALUES (?)", this->dbConnection);
                insertPath.BindText(0, context.path);
                insertPath.Step();
            }
        }
        else { /* remove old ones */
            db::Statement stmt("DELETE FROM paths WHERE path=?", this->dbConnection);
            stmt.BindText(0, context.path);
            stmt.Step();
        }

        this->addRemoveQueue.pop_front();
    }
}

void Indexer::RunAnalyzers() {
    typedef sdk::IAnalyzer PluginType;
    typedef PluginFactory::ReleaseDeleter<PluginType> Deleter;
    typedef std::shared_ptr<PluginType> PluginPtr;
    typedef std::vector<PluginPtr> PluginVector;

    /* short circuit if there aren't any analyzers */

    PluginVector analyzers = PluginFactory::Instance()
        .QueryInterface<PluginType, Deleter>("GetAudioAnalyzer");

    if (analyzers.empty()) {
        return;
    }

    /* for each track... */

    int64_t trackId = 0;

    db::Statement getNextTrack(
        "SELECT id FROM tracks WHERE id>? ORDER BY id LIMIT 1",
        this->dbConnection);

    getNextTrack.BindInt64(0, trackId);

    while(getNextTrack.Step() == db::Row ) {
        trackId = getNextTrack.ColumnInt64(0);

        getNextTrack.ResetAndUnbind();

        auto track = std::make_shared<IndexerTrack>(trackId);
        TrackMetadataQuery query(track, LibraryFactory::Instance().DefaultLocalLibrary());
        query.Run(this->dbConnection);

        if (query.GetStatus() == IQuery::Finished) {
            PluginVector runningAnalyzers;

            TagStore store(track);
            for (auto plugin : analyzers) {
                if (plugin->Start(&store)) {
                    runningAnalyzers.push_back(plugin);
                }
            }

            if (!runningAnalyzers.empty()) {
                audio::IStreamPtr stream = audio::Stream::Create(2048, 2.0, StreamFlags::NoDSP);

                if (stream) {
                    if (stream->OpenStream(track->Uri(), nullptr)) {

                        /* decode the stream quickly, passing to all analyzers */

                        IBuffer* buffer;

                        while ((buffer = stream->GetNextProcessedOutputBuffer()) && !runningAnalyzers.empty()) {
                            PluginVector::iterator plugin = runningAnalyzers.begin();
                            while(plugin != runningAnalyzers.end()) {
                                if ((*plugin)->Analyze(&store, buffer)) {
                                    ++plugin;
                                }
                                else {
                                    plugin = runningAnalyzers.erase(plugin);
                                }
                            }
                        }

                        /* done with track decoding and analysis, let the plugins know */

                        int successPlugins = 0;
                        PluginVector::iterator plugin = analyzers.begin();

                        for ( ; plugin != analyzers.end(); ++plugin) {
                            if ((*plugin)->End(&store)) {
                                successPlugins++;
                            }
                        }

                        /* the analyzers can write metadata back to the DB, so if any of them
                        completed successfully, then save the track. */

                        if (successPlugins>0) {
                            track->Save(this->dbConnection, this->libraryPath);
                        }
                    }
                }
            }
        }

        if (this->Bail()) {
            return;
        }

        getNextTrack.BindInt64(0, trackId);
    }
}

ITagStore* Indexer::CreateWriter() {
    return new TagStore(std::make_shared<IndexerTrack>(0));
}

bool Indexer::Save(IIndexerSource* source, ITagStore* store, const char* externalId) {
    if (!source || source->SourceId() == 0 || !store) {
        return false;
    }

    if (!externalId || strlen(externalId) == 0) {
        return false;
    }

    /* two levels of unpacking with dynamic_casts. don't tell anyone,
    it'll be our little secret. */
    TagStore* ts = dynamic_cast<TagStore*>(store);
    if (ts) {
        IndexerTrack* it = ts->As<IndexerTrack*>();
        if (it) {
            it->SetValue(constants::Track::EXTERNAL_ID, externalId);
            it->SetValue(constants::Track::SOURCE_ID, std::to_string(source->SourceId()).c_str());
            return it->Save(this->dbConnection, this->libraryPath);
        }
    }
    return false;
}

bool Indexer::RemoveByUri(IIndexerSource* source, const char* uri) {
    if (!source || source->SourceId() == 0) {
        return false;
    }

    if (!uri || strlen(uri) == 0) {
        return false;
    }

    db::Statement stmt(
        "DELETE FROM tracks WHERE source_id=? AND filename=?",
        this->dbConnection);

    stmt.BindInt32(0, source->SourceId());
    stmt.BindText(1, uri);

    return (stmt.Step() == db::Okay);
}

bool Indexer::RemoveByExternalId(IIndexerSource* source, const char* id) {
    if (!source || source->SourceId() == 0) {
        return false;
    }

    if (!id || strlen(id) == 0) {
        return false;
    }

    db::Statement stmt(
        "DELETE FROM tracks WHERE source_id=? AND external_id=?",
        this->dbConnection);

    stmt.BindInt32(0, source->SourceId());
    stmt.BindText(1, id);

    return (stmt.Step() == db::Okay);
}

int Indexer::RemoveAll(IIndexerSource* source) {
    if (!source) {
        return 0;
    }

    const auto id = source->SourceId();
    return (id != 0) ? this->RemoveAllForSourceId(id) : 0;
}

int Indexer::RemoveAllForSourceId(int sourceId) {
    db::Statement stmt("DELETE FROM tracks WHERE source_id=?", this->dbConnection);
    stmt.BindInt32(0, sourceId);
    return (stmt.Step() == db::Okay) ? dbConnection.LastModifiedRowCount() : 0;
}

void Indexer::CommitProgress(IIndexerSource* source, unsigned updatedTracks) {
    if (source &&
        this->currentSource &&
        this->currentSource->SourceId() == source->SourceId() &&
        trackTransaction)
    {
        trackTransaction->CommitAndRestart();
    }

    if (updatedTracks) {
        this->IncrementTracksScanned(updatedTracks);
    }
}

int Indexer::GetLastModifiedTime(IIndexerSource* source, const char* externalId) {
    if (source && externalId && strlen(externalId)) {
        db::Statement stmt("SELECT filetime FROM tracks t where source_id=? AND external_id=?", dbConnection);
        stmt.BindInt32(0, source->SourceId());
        stmt.BindText(1, externalId);
        if (stmt.Step() == db::Row) {
            return stmt.ColumnInt32(0);
        }
    }

    return -1;
}

void Indexer::ScheduleRescan(IIndexerSource* source) {
    if (source && source->SourceId() != 0) {
        this->Schedule(SyncType::Sources, source);
    }
}

bool Indexer::Bail() noexcept {
    return
        this->state == StateStopping ||
        this->state == StateStopped;
}
