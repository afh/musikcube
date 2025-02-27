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

#include <stdafx.h>

#include <musikcore/support/Common.h>
#include <cursespp/Text.h>
#include <cursespp/ScrollAdapterBase.h>
#include <cursespp/SingleLineEntry.h>

#include "DirectoryAdapter.h"

#include <filesystem>

namespace fs = std::filesystem;

using namespace musik::cube;
using namespace cursespp;

#ifdef WIN32
static const fs::path kDefaultRoot("");
#else
static const fs::path kDefaultRoot("/");
#endif

#ifdef WIN32
static void buildDriveList(std::vector<std::string>& target) {
    target.clear();
    static char buffer[4096];
    const DWORD result = ::GetLogicalDriveStringsA(4096, buffer);
    if (result && buffer) {
        char* current = buffer;
        while (*current) {
            target.push_back(std::string(current));
            current += strlen(current) + 1;
        }
    }
}

static bool shouldBuildDriveList(const fs::path& dir) {
    return dir.u8string().size() == 0;
}
#endif

static bool hasSubdirectories(fs::path p, bool showDotfiles) {
    try {
        fs::directory_iterator end;
        fs::directory_iterator file(p);

        while (file != end) {
            try {
                if (fs::is_directory(file->status())) {
                    if (showDotfiles || file->path().filename().u8string().at(0) != '.') {
                        return true;
                    }
                }
            }
            catch (...) {
                /* may throw trying to stat the file */
            }
            ++file;
        }
    }
    catch (...) {
        /* may throw trying to open the dir */
    }

    return false;
}

static void buildDirectoryList(
    const fs::path& p,
    std::vector<std::string>& target,
    bool showDotfiles)
{
    target.clear();

    try {
        fs::directory_iterator end;
        fs::directory_iterator file(p);

        while (file != end) {
            try {
                if (is_directory(file->status())) {
                    std::string leaf = file->path().filename().u8string();
                    if (showDotfiles || leaf.at(0) != '.') {
                        target.push_back(file->path().u8string());
                    }
                }
            }
            catch (...) {
                /* may throw trying to stat the file */
            }
            ++file;
        }
    }
    catch (...) {
        /* may throw trying to open the directory */
    }

    try {
        std::sort(
            target.begin(),
            target.end(),
            std::locale(setlocale(LC_ALL, nullptr)));
    }
    catch (...) {
        std::sort(target.begin(), target.end());
    }
}

static std::string pathToString(fs::path path) {
    return musik::core::NormalizeDir(path.u8string());
}

static fs::path stringToPath(const std::string& path) {
    return fs::u8path(musik::core::NormalizeDir(path));
}

DirectoryAdapter::DirectoryAdapter() {
    this->showDotfiles = false;
    this->showRootDirectory = false;
    this->dir = fs::path(fs::u8path(musik::core::GetHomeDirectory()));
    this->rootDir = kDefaultRoot;
    buildDirectoryList(dir, subdirs, showDotfiles);
}

DirectoryAdapter::~DirectoryAdapter() {
}

void DirectoryAdapter::SetAllowEscapeRoot(bool allow) {
    this->allowEscapeRoot = allow;
}

void DirectoryAdapter::SetShowRootDirectory(bool show) {
    if (show != this->showRootDirectory) {
        this->showRootDirectory = show;
        this->Refresh();
    }
}

size_t DirectoryAdapter::Select(cursespp::ListWindow* window) {
    const bool hasParent = this->ShowParentPath();
    size_t selectedIndex = NO_INDEX;
    size_t initialIndex = window->GetSelectedIndex();
    if (this->IsCurrentDirectory(initialIndex)) {
        return initialIndex;
    }
    if (hasParent && initialIndex == 0) {
        if (selectedIndexStack.size()) {
            selectedIndex = this->selectedIndexStack.top();
            this->selectedIndexStack.pop();
        }
#ifdef WIN32
        /* has_relative_path() is weird, as it does what i'd expect
        has_parent_path() to do... documentation doesn't really help
        much, but basically we're checking if we're at the root here;
        if we are, we reset the directory to empty so we can build a
        drive list. */
        if (!this->dir.has_relative_path()) {
            this->dir = "";
        }
        else {
            this->dir = this->dir.parent_path();
        }
#else
        this->dir = this->dir.parent_path();
#endif
    }
    else {
        selectedIndexStack.push(initialIndex);
        initialIndex -= this->GetHeaderCount();
        this->dir = fs::u8path(this->subdirs.at(initialIndex));
    }

#ifdef WIN32
    if (shouldBuildDriveList(this->dir)) {
        dir = fs::path();
        buildDriveList(subdirs);
        return selectedIndex;
    }
#endif

    buildDirectoryList(dir, subdirs, showDotfiles);
    window->OnAdapterChanged();

    return selectedIndex;
}

void DirectoryAdapter::SetRootDirectory(const std::string& directory) {
    if (directory.size()) {
        dir = rootDir = stringToPath(directory);
    }
    else {
        dir = musik::core::GetHomeDirectory();
        rootDir = kDefaultRoot;
    }
    buildDirectoryList(dir, subdirs, showDotfiles);
}

std::string DirectoryAdapter::GetFullPathAt(size_t index) {
    const bool hasParent = this->ShowParentPath();
    if (hasParent && index == 0) {
        return "";
    }
    if (this->IsCurrentDirectory(index)) {
        return this->dir.u8string();
    }
    index -= this->GetHeaderCount();
    return pathToString(fs::u8path(this->subdirs.at(index)));
}

std::string DirectoryAdapter::GetLeafAt(size_t index) {
    if (this->ShowParentPath() && index == 0) {
        return "..";
    }
    if (this->IsCurrentDirectory(index)) {
        return ".";
    }
    return fs::u8path(this->subdirs.at(index)).u8string();
}

size_t DirectoryAdapter::IndexOf(const std::string& leaf) {
   for (size_t i = 0; i < this->subdirs.size(); i++) {
        if (this->subdirs.at(i) == leaf) {
            return i + this->GetHeaderCount();
        }
    }
    return NO_INDEX;
}

size_t DirectoryAdapter::GetEntryCount() {
    return this->GetHeaderCount() + subdirs.size();
}

void DirectoryAdapter::SetDotfilesVisible(bool visible) {
    if (showDotfiles != visible) {
        showDotfiles = visible;
#ifdef WIN32
        if (shouldBuildDriveList(this->dir)) {
            buildDriveList(subdirs);
            return;
        }
#endif
        buildDirectoryList(dir, subdirs, showDotfiles);
    }
}

std::string DirectoryAdapter::GetParentPath() {
    if (dir.has_parent_path() &&
        pathToString(this->dir) != pathToString(this->rootDir))
    {
        return pathToString(dir.parent_path().u8string());
    }

    return "";
}

std::string DirectoryAdapter::GetCurrentPath() {
    return pathToString(dir.u8string());
}

void DirectoryAdapter::Refresh() {
    buildDirectoryList(dir, subdirs, showDotfiles);
}

bool DirectoryAdapter::IsAtRoot() {
    return pathToString(this->dir) == pathToString(this->rootDir);
}

bool DirectoryAdapter::ShowParentPath() {
    if (this->IsAtRoot() && !this->allowEscapeRoot) {
        return false;
    }
    return dir.has_parent_path();
}

bool DirectoryAdapter::ShowCurrentDirectory() {
    return this->showRootDirectory && this->IsAtRoot();
}

size_t DirectoryAdapter::GetHeaderCount() {
    return (this->ShowParentPath() ? 1 : 0) + (this->ShowCurrentDirectory() ? 1 : 0);
}

bool DirectoryAdapter::HasSubDirectories(size_t index) {
    const bool hasParent = this->ShowParentPath();
    if (index == 0 && hasParent) {
        return true;
    }
    if (this->IsCurrentDirectory(index)) {
        return !this->subdirs.empty();
    }
    index -= this->GetHeaderCount();
    return hasSubdirectories(fs::u8path(this->subdirs.at(index)), this->showDotfiles);
}

bool DirectoryAdapter::HasSubDirectories() {
    return hasSubdirectories(this->dir, this->showDotfiles);
}

bool DirectoryAdapter::IsCurrentDirectory(size_t index) {
    if (!this->showRootDirectory) {
        return false;
    }
    auto const showParent = this->ShowParentPath();
    return !showParent && index == 0;
}

IScrollAdapter::EntryPtr DirectoryAdapter::GetEntry(cursespp::ScrollableWindow* window, size_t index) {
    if (this->ShowParentPath()) {
        if (index == 0) {
            return IScrollAdapter::EntryPtr(new SingleLineEntry(".."));
        }
    }
    else if (this->ShowCurrentDirectory()) {
        if (IsCurrentDirectory(index)) {
            const auto dir = u8fmt("[%s]", + this->rootDir.parent_path().filename().u8string().c_str());
            return IScrollAdapter::EntryPtr(new SingleLineEntry(dir));
        }
    }
    index -= this->GetHeaderCount();
    auto path = fs::u8path(this->subdirs[index]);
    auto pathString = path.filename().u8string();
    /* windows root drives are 'x:\', so they don't have a filename leaf. if
    we resolve an empty string, just use the whole path */
    if (!pathString.size()) {
        pathString = path.u8string();
    }
    auto text = text::Ellipsize(pathString, this->GetWidth());
    return IScrollAdapter::EntryPtr(new SingleLineEntry(text));
}
