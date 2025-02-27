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

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <ctime>
#include <cstdint>
#include <filesystem>

class LruDiskCache {
    public:
        LruDiskCache();

        void Purge();

        bool Finalize(size_t id, int64_t instanceId, std::string type);
        FILE* Open(size_t id, int64_t instanceId, const std::string& mode);
        FILE* Open(size_t id, int64_t instanceId, const std::string& mode, std::string& type, size_t& len);
        bool Cached(size_t id);
        void Delete(size_t id, int64_t instanceId);
        void Touch(size_t id);

        void Init(const std::string& root, size_t maxEntries);

    private:
        struct Entry {
            uint64_t id;
            std::string path;
            std::string type;
            std::filesystem::file_time_type time;
        };

        using EntryPtr = std::shared_ptr<Entry>;
        using EntryList = std::vector<EntryPtr>;

        void SortAndPrune();

        static std::shared_ptr<Entry> Parse(const std::filesystem::path& path);

        std::recursive_mutex stateMutex;

        bool initialized;
        size_t maxEntries;
        EntryList cached;
        std::string root;
};
