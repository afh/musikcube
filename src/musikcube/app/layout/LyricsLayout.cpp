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
#include <app/layout/LyricsLayout.h>
#include <app/util/Messages.h>
#include <musikcore/i18n/Locale.h>
#include <musikcore/support/Auddio.h>
#include <musikcore/sdk/String.h>
#include <musikcore/library/query/LyricsQuery.h>
#include <cursespp/App.h>
#include <cursespp/Screen.h>
#include <cursespp/ToastOverlay.h>
#include <cursespp/SingleLineEntry.h>
#include <app/util/Hotkeys.h>
#include <app/util/Messages.h>

using namespace musik::cube;
using namespace musik::core;
using namespace musik::core::audio;
using namespace musik::core::library::query;
using namespace musik::core::runtime;
using namespace musik::core::sdk;
using namespace cursespp;

LyricsLayout::LyricsLayout(PlaybackService& playback, ILibraryPtr library)
: LayoutBase()
, currentTrackId(-1LL)
, library(library)
, playback(playback) {
    this->playback.TrackChanged.connect(this, &LyricsLayout::OnTrackChanged);

    this->adapter = std::make_shared<SimpleScrollAdapter>();
    this->adapter->SetSelectable(true);

    this->listView = std::make_shared<ListWindow>(this->adapter);
    this->AddWindow(this->listView);
    this->listView->SetFocusOrder(0);

    this->infoText = std::make_shared<TextLabel>("", text::AlignCenter);
    this->AddWindow(this->infoText);

    this->LoadLyricsForCurrentTrack();
}

void LyricsLayout::OnLayout() {
    LayoutBase::OnLayout();
    const int cx = this->GetContentWidth();
    const int cy = this->GetContentHeight();
    this->listView->MoveAndResize(0, 0, cx, cy);
    this->infoText->MoveAndResize(1, cy / 2, cx - 2, 1);
}

void LyricsLayout::OnTrackChanged(size_t index, TrackPtr track) {
    if (this->IsVisible()) {
        this->LoadLyricsForCurrentTrack();
    }
}

void LyricsLayout::OnLyricsLoaded() {
    this->UpdateAdapter();
    this->listView->ScrollTo(0);
    this->listView->SetSelectedIndex(0);

    auto track = this->playback.GetPlaying();
    if (track) {
        this->listView->SetFrameTitle(u8fmt(
            _TSTR("lyrics_list_title"),
            track->GetString("title").c_str(),
            track->GetString("artist").c_str()));
    }

    this->SetState(State::Loaded);
}

bool LyricsLayout::KeyPress(const std::string& kn) {
    if (Hotkeys::Is(Hotkeys::LyricsRetry, kn)) {
        this->LoadLyricsForCurrentTrack();
        return true;
    }
    else if (Hotkeys::Is(Hotkeys::NavigateLibraryPlayQueue, kn)) {
        this->Broadcast(message::JumpToPlayQueue);
        return true;
    }
    else if (Hotkeys::Is(Hotkeys::NavigateLibrary, kn)) {
        this->Broadcast(message::JumpToLibrary);
        return true;
    }
    else if (kn == " ") { /* ugh... need to generalize this maybe */
        playback.PauseOrResume();
        return true;
    }

    return LayoutBase::KeyPress(kn);
}

void LyricsLayout::OnVisibilityChanged(bool visible) {
    LayoutBase::OnVisibilityChanged(visible);
    if (visible) {
        this->LoadLyricsForCurrentTrack();
        this->FocusFirst();
    }
}

#include <iostream>

void LyricsLayout::ProcessMessage(musik::core::runtime::IMessage &m) {
    if (m.Type() == message::LyricsLoaded) {
        const auto state = static_cast<State>(m.UserData1());
        if (state == State::Loaded && this->currentLyrics.size()) {
            this->OnLyricsLoaded();
        }
        else {
            this->SetState(state);
        }
    }
    else {
        LayoutBase::ProcessMessage(m);
    }
}

void LyricsLayout::LoadLyricsForCurrentTrack() {
    auto track = playback.GetPlaying();
    if (track && track->GetId() != this->currentTrackId) {
        this->currentTrackId = track->GetId();
        this->currentLyrics = "";
        this->SetState(State::Loading);
        const auto trackExternalId = track->GetString("external_id");
        auto lyricsDbQuery = std::make_shared<LyricsQuery>(trackExternalId);
        this->library->Enqueue(lyricsDbQuery, [this, lyricsDbQuery, track](auto q) {
            auto localLyrics = lyricsDbQuery->GetResult();
            if (localLyrics.size()) {
                this->currentLyrics = localLyrics;
                this->Post(message::LyricsLoaded, static_cast<int64_t>(State::Loaded));
            }
            else {
                auddio::FindLyrics(track, [this](TrackPtr track, std::string remoteLyrics) {
                    if (this->currentTrackId == track->GetId()) {
                        this->currentLyrics = remoteLyrics;
                        const auto state = remoteLyrics.size() ? State::Loaded : State::Failed;
                        this->Post(message::LyricsLoaded, static_cast<int64_t>(state));
                    }
                });
            }
        });
    }
    else if (!track) {
        this->currentTrackId = -1LL;
        this->SetState(State::NotPlaying);
    }
}

void LyricsLayout::UpdateAdapter() {
    std::string fixed = this->currentLyrics;
    str::ReplaceAll(fixed, "\r\n", "\n");
    str::ReplaceAll(fixed, "\r", "\n");
    auto items = str::Split(fixed, "\n");
    this->adapter->Clear();
    for (auto& text : items) {
        this->adapter->AddEntry(std::make_shared<SingleLineEntry>(text));
    }
}

void LyricsLayout::SetState(State state) {
    switch (state) {
        case State::NotPlaying: {
                this->listView->Hide();
                this->infoText->Show();
                this->infoText->SetText(_TSTR("lyrics_not_playing"));
                this->currentTrackId = -1LL;
            }
            break;
        case State::Loading: {
                this->listView->Hide();
                this->infoText->Show();
                this->infoText->SetText(_TSTR("lyrics_loading"));
            }
            break;
        case State::Loaded: {
                this->infoText->Hide();
                this->listView->Show();
                if (this->IsVisible()) {
                    this->listView->Focus();
                }
            }
            break;
        case State::Failed: {
                this->listView->Hide();
                this->infoText->Show();
                this->infoText->SetText(u8fmt(
                    _TSTR("lyrics_lookup_failed"),
                    Hotkeys::Get(Hotkeys::LyricsRetry).c_str()));
                this->currentTrackId = -1LL;
            }
            break;
    }
}

void LyricsLayout::SetShortcutsWindow(ShortcutsWindow* shortcuts) {
    if (shortcuts) {
        shortcuts->AddShortcut(Hotkeys::Get(Hotkeys::NavigateLyrics), _TSTR("shortcuts_lyrics"));
        shortcuts->AddShortcut(Hotkeys::Get(Hotkeys::NavigateLibrary), _TSTR("shortcuts_library"));
        shortcuts->AddShortcut(Hotkeys::Get(Hotkeys::NavigateLibraryPlayQueue), _TSTR("shortcuts_play_queue"));
        shortcuts->AddShortcut(App::Instance().GetQuitKey(), _TSTR("shortcuts_quit"));

        shortcuts->SetChangedCallback([this](std::string key) {
            if (key == App::Instance().GetQuitKey()) {
                App::Instance().Quit();
            }
            else {
                this->KeyPress(key);
            }
        });

        shortcuts->SetActive(Hotkeys::Get(Hotkeys::NavigateLyrics));
    }
}
