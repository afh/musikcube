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

#include <cursespp/ScrollAdapterBase.h>
#include <cursespp/ListWindow.h>

#include <filesystem>
#include <vector>
#include <stack>

namespace musik {
    namespace cube {
        class DirectoryAdapter : public cursespp::ScrollAdapterBase {
            public:
                static const size_t NO_INDEX = (size_t)-1;

                DirectoryAdapter();
                virtual ~DirectoryAdapter();

                size_t Select(cursespp::ListWindow* window);
                std::string GetParentPath();
                std::string GetCurrentPath();
                std::string GetFullPathAt(size_t index);
                std::string GetLeafAt(size_t index);
                bool HasSubDirectories(size_t index);
                bool HasSubDirectories();
                void SetRootDirectory(const std::string& fullPath);
                void SetAllowEscapeRoot(bool allowEscape);
                size_t IndexOf(const std::string& leaf);
                void SetDotfilesVisible(bool visible);
                void SetShowRootDirectory(bool showRootDirectory);
                bool IsAtRoot();
                void Refresh();

                /* ScrollAdapterBase */
                size_t GetEntryCount() override;
                EntryPtr GetEntry(cursespp::ScrollableWindow* window, size_t index) override;

            private:
                bool ShowParentPath();
                bool ShowCurrentDirectory();
                bool IsCurrentDirectory(size_t index);
                size_t GetHeaderCount();

                std::filesystem::path dir, rootDir;
                std::vector<std::string> subdirs;
                std::stack<size_t> selectedIndexStack;
                bool showDotfiles, allowEscapeRoot, showRootDirectory;
        };
    }
}
