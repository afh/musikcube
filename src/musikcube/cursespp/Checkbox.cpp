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

#include <cursespp/Screen.h>
#include <cursespp/Colors.h>
#include <cursespp/Text.h>
#include <cursespp/Checkbox.h>

using namespace cursespp;

static const std::string UNCHECKED = "[ ] ";
static const std::string CHECKED = "[x] ";

static std::string decorate(const std::string& str, bool checked) {
    return (checked ? CHECKED : UNCHECKED) + str;
}

Checkbox::Checkbox()
: TextLabel()
, checked(false) {
}

Checkbox::Checkbox(const std::string& value)
: TextLabel(decorate(value, false))
, originalText(value) {
}

Checkbox::Checkbox(const std::string& value, const text::TextAlign alignment)
: TextLabel(decorate(value, false), alignment)
, originalText(value) {
}

Checkbox::~Checkbox() {
}

void Checkbox::SetText(const std::string& value) {
    if (value != this->originalText) {
        this->originalText = value;
        TextLabel::SetText(decorate(value, this->checked));
    }
}

std::string Checkbox::GetText() {
    return this->originalText;
}

void Checkbox::SetChecked(bool checked) {
    if (checked != this->checked) {
        this->checked = checked;
        TextLabel::SetText(decorate(this->originalText, checked));
        this->CheckChanged(this, checked);
    }
}

bool Checkbox::KeyPress(const std::string& key) {
    if (key == " " || key == "KEY_ENTER") {
        this->SetChecked(!this->checked);
        return true;
    }
    return false;
}

bool Checkbox::ProcessMouseEvent(const IMouseHandler::Event& event) {
    if (event.Button1Clicked()) {
        this->FocusInParent();
        this->SetChecked(!this->checked);
        return true;
    }
    return TextLabel::ProcessMouseEvent(event);
}