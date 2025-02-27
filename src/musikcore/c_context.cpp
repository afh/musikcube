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

#include <pch.hpp>

#include <musikcore/musikcore_c.h>
#include <musikcore/c_context.h>
#include <musikcore/debug.h>
#include <musikcore/runtime/MessageQueue.h>
#include <musikcore/runtime/Message.h>
#include <musikcore/library/LibraryFactory.h>
#include <musikcore/library/LocalLibrary.h>
#include <musikcore/audio/PlaybackService.h>
#include <musikcore/plugin/Plugins.h>
#include <musikcore/library/LocalMetadataProxy.h>
#include <musikcore/support/PreferenceKeys.h>

#include <thread>

using namespace musik;
using namespace musik::core;
using namespace musik::core::library;;
using namespace musik::core::library::query;
using namespace musik::core::audio;
using namespace musik::core::sdk;
using namespace musik::core::runtime;

/*
 * mcsdk_context_message_queue
 */

mcsdk_context_message_queue::mcsdk_context_message_queue(): MessageQueue() {
    this->quit = false;
}

mcsdk_context_message_queue::~mcsdk_context_message_queue() {
}

void mcsdk_context_message_queue::Quit() {
    {
        LockT lock(this->mutex);
        this->quit = true;
    }
    this->Post(Message::Create(0, 0, 0, 0));
}

void mcsdk_context_message_queue::Run() {
    while (true) {
        this->WaitAndDispatch();
        {
            LockT lock(this->mutex);
            if (this->quit) {
                return;
            }
        }
    }
}

/*
 * globals
 */

static std::recursive_mutex global_mutex;
static bool environment_initialized = false;
static mcsdk_context* plugin_context = nullptr;
static mcsdk_context_message_queue* message_queue = nullptr;
static std::thread message_queue_thread;

/*
 * mcsdk_svc_indexer_callback_proxy
 */

struct mcsdk_svc_indexer_callback_proxy: public sigslot::has_slots<> {
    mcsdk_svc_indexer_context_internal* context;

    mcsdk_svc_indexer_callback_proxy(mcsdk_svc_indexer_context_internal* context) {
        this->context = context;
    }

    void on_started() {
        for (auto cb : context->callbacks) {
            if (cb->on_started) {
                cb->on_started(mcsdk_svc_indexer { context });
            }
        }
    }

    void on_finished(int tracks_processed) {
        for (auto cb : context->callbacks) {
            if (cb->on_finished) {
                cb->on_finished(mcsdk_svc_indexer { context }, tracks_processed);
            }
        }
    }

    void on_progress(int tracks_processed) {
        for (auto cb : context->callbacks) {
            if (cb->on_progress) {
                cb->on_progress(mcsdk_svc_indexer { context }, tracks_processed);
            }
        }
    }
};

mcsdk_export void mcsdk_env_init() {
    std::unique_lock<std::recursive_mutex> lock(global_mutex);

    if (!environment_initialized) {
        debug::Start();
        message_queue = new mcsdk_context_message_queue();
        message_queue_thread = std::thread([]{ /* needs to be last */
            ::message_queue->Run();
        });
        environment_initialized = true;
    }
}

mcsdk_export void mcsdk_env_release() {
    if (environment_initialized) {
        LibraryFactory::Instance().Shutdown();
        debug::Shutdown();
        message_queue->Quit();
        message_queue_thread.join();
        delete message_queue;
        message_queue = nullptr;
        environment_initialized = false;
    }
}

/*
 * mcsdk_context_*
 */

mcsdk_export void mcsdk_context_init(mcsdk_context** context) {
    std::unique_lock<std::recursive_mutex> lock(global_mutex);

    if (!environment_initialized) {
        mcsdk_env_init();
    }

    plugin::Init();

    auto c = new mcsdk_context();
    memset(c, 0, sizeof(mcsdk_context));

    auto internal = new mcsdk_context_internal();

    LibraryFactory::Initialize(*message_queue);
    internal->library = LibraryFactory::Instance().DefaultLocalLibrary();
    internal->playback = new PlaybackService(*message_queue, internal->library);
    internal->metadata = new LocalMetadataProxy(internal->library);
    internal->preferences = Preferences::ForComponent(prefs::components::Settings);

    c->internal.opaque = internal;
    c->metadata.opaque = internal->metadata;
    c->preferences.opaque = internal->preferences.get();
    c->playback.opaque = internal->playback;
    c->library.opaque = internal->library.get();

    auto localLibrary = dynamic_cast<LocalLibrary*>(internal->library.get());
    if (localLibrary) {
        c->db.opaque = localLibrary;
    }

    auto indexer = internal->library->Indexer();
    auto indexer_internal = new mcsdk_svc_indexer_context_internal();
    indexer_internal->indexer = indexer;
    indexer_internal->callback_proxy = new mcsdk_svc_indexer_callback_proxy(indexer_internal);
    indexer->Started.connect(indexer_internal->callback_proxy, &mcsdk_svc_indexer_callback_proxy::on_started);
    indexer->Progress.connect(indexer_internal->callback_proxy, &mcsdk_svc_indexer_callback_proxy::on_progress);
    indexer->Finished.connect(indexer_internal->callback_proxy, &mcsdk_svc_indexer_callback_proxy::on_finished);
    c->indexer.opaque = indexer_internal;

    if (!plugin_context) {
        mcsdk_set_plugin_context(c);
    }

    *context = c;
}

mcsdk_export void mcsdk_context_release(mcsdk_context** context) {
    std::unique_lock<std::recursive_mutex> lock(global_mutex);

    auto c = *context;
    auto internal = static_cast<mcsdk_context_internal*>(c->internal.opaque);

    delete internal->playback;

    internal->playback = nullptr;
    internal->library->Indexer()->Shutdown();
    internal->library.reset();
    internal->preferences.reset();

    delete internal->metadata;

    auto indexer_internal = static_cast<mcsdk_svc_indexer_context_internal*>(c->indexer.opaque);
    delete indexer_internal->callback_proxy;
    delete indexer_internal;

    delete internal;

    if (plugin_context == c) {
        mcsdk_set_plugin_context(nullptr);
    }

    delete c;

    *context = 0;
}

mcsdk_export void mcsdk_set_plugin_context(mcsdk_context* context) {
    if (plugin_context && plugin_context != context) {
        plugin::Shutdown();
    }
    plugin_context = context;
    if (plugin_context) {
        auto internal = static_cast<mcsdk_context_internal*>(context->internal.opaque);
        plugin::Start(message_queue, internal->playback, internal->library);
    }
}

mcsdk_export bool mcsdk_is_plugin_context(mcsdk_context* context) {
    return context && context == plugin_context;
}