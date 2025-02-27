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

#pragma once

#include <musikcore/sdk/IMap.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <functional>

namespace musik { namespace core {

    class MetadataMap :
        public musik::core::sdk::IMap,
        public std::enable_shared_from_this<MetadataMap>
    {
        public:
            MetadataMap(
                int64_t id,
                const std::string& value,
                const std::string& type);

            virtual ~MetadataMap();

            /* IResource */
            virtual int64_t GetId();
            virtual musik::core::sdk::IResource::Class GetClass();
            virtual const char* GetType();

            /* IValue */
            virtual size_t GetValue(char* dst, size_t size);

            /* IMap */
            virtual void Release();
            virtual int GetString(const char* key, char* dst, int size);
            virtual long long GetInt64(const char* key, long long defaultValue = 0LL);
            virtual int GetInt32(const char* key, unsigned int defaultValue = 0);
            virtual double GetDouble(const char* key, double defaultValue = 0.0f);

            /* implementation specific */
            void Set(const char* key, const std::string& value);
            std::string Get(const char* key);
            std::string GetTypeValue();
            musik::core::sdk::IMap* GetSdkValue();
            void Each(std::function<void(const std::string&, const std::string&)> callback);

        private:
            int64_t id;
            std::string type, value;
            std::unordered_map<std::string, std::string> metadata;
    };

    using MetadataMapPtr = std::shared_ptr<MetadataMap>;

} }
