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

#include "config.h"

#include "LruDiskCache.h"

#include <musikcore/sdk/String.h>

#include <algorithm>
#include <filesystem>
#include <chrono>

const std::string PREFIX = "musikcube";
const std::string TEMP_EXTENSION = ".tmp";

namespace fs = std::filesystem;
using namespace musik::core::sdk;

using Lock = std::unique_lock<std::recursive_mutex>;

static std::string tempFilename(const std::string& root, size_t id, int64_t instanceId) {
    return root + "/" + PREFIX + "_" + std::to_string(id) + "_" + std::to_string(instanceId) + TEMP_EXTENSION;
}

static std::string finalFilename(const std::string& root, size_t id, std::string type) {
    str::ReplaceAll(type, "/", "-");
    return root + "/" + PREFIX + "_" + std::to_string(id) + "_" + type;
}

static bool isTemp(const fs::path& path) {
    return path.extension().u8string() == TEMP_EXTENSION;
}

static bool isTemp(const std::string& path) {
    return isTemp(fs::path(fs::u8path(path)));
}

static std::filesystem::file_time_type touch(const std::string& path) {
    auto fsPath = fs::u8path(path);
    try {
        std::chrono::time_point now = fs::file_time_type::clock::now();
        fs::last_write_time(fsPath, now);
    }
    catch (...) {
    }

    return fs::last_write_time(fsPath);
}

static bool rm(const std::string& path) {
    try {
        return fs::remove(fs::path(path));
    }
    catch (...) {

    }

    return false;
}

static bool rm(const fs::path& p) {
    return rm(p.u8string());
}

LruDiskCache::LruDiskCache()
: maxEntries(10)
, initialized(false) {

}

void LruDiskCache::Init(const std::string& root, size_t maxEntries) {
    Lock lock(this->stateMutex);

    if (!this->initialized) {
        this->initialized = true;
        this->root = root;
        this->maxEntries = maxEntries;

        this->Purge(); /* always purge partial files on startup */

        /* index all the completed files... */
        std::error_code ec;
        fs::directory_iterator end;
        fs::directory_iterator file(fs::u8path(this->root), ec);

        while (file != end) {
            if (!is_directory(file->status())) {
                if (!isTemp(file->path())) {
                    auto entry = LruDiskCache::Parse(file->path());
                    if (entry) {
                        this->cached.push_back(entry);
                    }
                }
            }
            ++file;
        }

        this->SortAndPrune();
    }
}

void LruDiskCache::Purge() {
    Lock lock(stateMutex);

    std::error_code ec;
    fs::directory_iterator end;
    fs::directory_iterator file(fs::u8path(this->root), ec);

    while (file != end) {
        if (!is_directory(file->status())) {
            if (isTemp(file->path())) {
                rm(file->path());
            }
        }
        ++file;
    }
}

LruDiskCache::EntryPtr LruDiskCache::Parse(const fs::path& path) {
    std::string fn = path.stem().u8string() + path.extension().u8string();
    std::vector<std::string> parts = str::Split(fn, "_");
    if (parts.size() == 3 && parts[0] == PREFIX) {
        try {
            auto entry = std::shared_ptr<Entry>(new Entry());
            entry->id = std::stoull(parts[1].c_str());
            entry->path = path.u8string();
            entry->type = parts[2];
            entry->time = fs::last_write_time(path);
            str::ReplaceAll(entry->type, "-", "/");
            return entry;
        }
        catch (...) {
            /* can't parse. it's invalid. */
        }
    }

    return EntryPtr();
}

bool LruDiskCache::Finalize(size_t id, int64_t instanceId, std::string type) {
    Lock lock(stateMutex);

    if (type.size() == 0) {
        type = "unknown";
    }

    fs::path src(tempFilename(this->root, instanceId, id));
    fs::path dst(finalFilename(this->root, id, type));

    if (fs::exists(src)) {
        if (fs::exists(dst)) {
            if (!rm(dst)) {
                return false;
            }
        }

        try {
            fs::rename(src, dst);
        }
        catch (...) {
            return false;
        }

        auto entry = LruDiskCache::Parse(dst);
        if (entry) {
            this->cached.push_back(entry);
            this->SortAndPrune();
        }
    }

    return true;
}

bool LruDiskCache::Cached(size_t id) {
    Lock lock(stateMutex);

    auto end = this->cached.end();
    auto it = std::find_if(this->cached.begin(), end, [id](EntryPtr entry) {
        return entry->id == id;
    });

    return it != end;
}

FILE* LruDiskCache::Open(size_t id, int64_t instanceId, const std::string& mode) {
    std::string type;
    size_t len;
    return this->Open(id, instanceId, mode, type, len);
}

FILE* LruDiskCache::Open(size_t id, int64_t instanceId, const std::string& mode, std::string& type, size_t& len) {
    Lock lock(stateMutex);

    auto end = this->cached.end();
    auto it = std::find_if(this->cached.begin(), end, [id](EntryPtr entry) {
        return entry->id == id;
    });

    FILE* result = nullptr;

    if (it != end) {
        result = fopen((*it)->path.c_str(), mode.c_str());

        if (result) {
            type = (*it)->type;
            fseek(result, 0, SEEK_END);
            len = (size_t) ftell(result);
            fseek(result, 0, SEEK_SET);
        }

        this->Touch(id);
    }

    if (result) {
        return result;
    }

    /* ensure the cache directory exists */
    std::error_code ec;
    fs::path p(fs::u8path(this->root));
    if (!fs::exists(p)) {
        fs::create_directories(p, ec);
    }

    /* open the file and return it regardless of cache status. */
    return fopen(tempFilename(this->root, instanceId, id).c_str(), mode.c_str());
}

void LruDiskCache::Delete(size_t id, int64_t instanceId) {
    Lock lock(stateMutex);

    auto it = this->cached.begin();
    while (it != this->cached.end()) {
        if ((*it)->id == id) {
            rm((*it)->path);
            return;
        }
        else {
            ++it;
        }
    }

    rm(tempFilename(this->root, id, instanceId));
}

void LruDiskCache::SortAndPrune() {
    Lock lock(this->stateMutex);

    std::sort( /* sort by access time */
        this->cached.begin(),
        this->cached.end(),
        [](EntryPtr e1, EntryPtr e2) {
            return e1->time > e2->time;
        });

    /* prune old entries */
    int count = (int) this->cached.size();
    int extras = count - this->maxEntries;
    for (int i = 0; i < extras; i++) {
        auto entry = this->cached.back();
        if (rm(entry->path)) {
            this->cached.pop_back();
        }
    }
}

void LruDiskCache::Touch(size_t id) {
    Lock lock(this->stateMutex);

    auto end = this->cached.end();
    auto it = std::find_if(this->cached.begin(), end, [id](EntryPtr entry) {
        return entry->id == id;
    });

    if (it != end) {
        auto e = (*it);
        fs::path p(e->path);
        if (fs::exists(p)) {
            e->time = touch(p.u8string());
            this->SortAndPrune();
            return;
        }
    }
}