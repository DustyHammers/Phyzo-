#include "skin_elements.h"
#include <RmlUi/Core.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "skin_view.h"

namespace skin {

int parseInt(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 0);       // base 0: "0x1A" or "26"
    return end && *end == 0 ? int(v) : fallback;
}

namespace {
std::string attr(Rml::Element& e, const char* name) { return e.GetAttribute<Rml::String>(name, ""); }

std::vector<std::unique_ptr<Rml::ElementInstancer>>& instancers() {
    static std::vector<std::unique_ptr<Rml::ElementInstancer>> v;
    return v;
}
template <class T> void add(const char* tag) {
    instancers().push_back(std::make_unique<Rml::ElementInstancerGeneric<T>>());
    Rml::Factory::RegisterElementInstancer(tag, instancers().back().get());
}
}  // namespace

void registerElements() {
    add<KnobElement>("knob");
    add<PButtonElement>("pbutton");
    add<LedElement>("led");
    add<VfDigitElement>("vfdigit");
}

// ------------------------------------------------------------------ sprites

std::string SpriteElement::prefix() {
    const Rml::Property* p = GetProperty("spriteprefix");
    return p ? p->Get<Rml::String>() : std::string();
}

void SpriteElement::showSprite(int index) {
    const std::string pre = prefix();
    if (pre.empty()) return;
    char num[8];
    std::snprintf(num, sizeof num, "%03d", std::clamp(index, 0, 999));
    const std::string want = "image(" + pre + num + ")";
    if (want == shown_) return;
    shown_ = want;
    SetProperty("decorator", want);
}

// ------------------------------------------------------------------ knob

KnobElement::KnobElement(const Rml::String& tag) : SpriteElement(tag), view_(SkinView::current()) {
    if (view_) view_->registerElement(this);
}
KnobElement::~KnobElement() { if (view_) view_->unregisterElement(this); }

void KnobElement::OnAttributeChange(const Rml::ElementAttributes& changed) {
    SpriteElement::OnAttributeChange(changed);
    if (changed.count("cc") || changed.count("id")) {
        cc_ = parseInt(attr(*this, "cc"), -1);
        // Each knob element keeps its own position (stacked knobs share a cc): the stored one, else the value
        // last sent for its cc (the F-02 defaults in a fresh instance), remembered from now on.
        if (view_ && cc_ >= 0 && cc_ < 26) {
            int v = 0;
            show(view_->port().knobPosition(key(), v) ? v : view_->port().controlPosition(cc_));
        }
    }
}

std::string KnobElement::key() const {
    const std::string id = GetId();
    return id.empty() ? "cc" + std::to_string(cc_) : id;
}

void KnobElement::OnUpdate() {
    SpriteElement::OnUpdate();
    redraw();
}

void KnobElement::redraw() {
    const Rml::Property* p = GetProperty("frames");
    const int frames = p ? std::max(1, int(std::lround(p->Get<float>()))) : 1;
    showSprite(int(std::lround(double(raw_) * (frames - 1) / 1023.0)));
}

void KnobElement::show(int raw) {
    raw_ = std::clamp(raw, 0, 1023);
    known_ = true;
    if (view_) view_->port().setKnobPosition(key(), raw_);
    redraw();
}

void KnobElement::move(int raw) {
    raw = std::clamp(raw, 0, 1023);
    if (known_ && raw == raw_) return;
    show(raw);
    if (view_ && cc_ >= 0 && cc_ < 26) view_->knobSent(cc_, raw_);
}

void KnobElement::wheel(float notches, bool fine) {
    move(raw_ + int(std::lround(notches * (fine ? 1.0 : 16.0))));
}

// ------------------------------------------------------------------ panel button

PButtonElement::PButtonElement(const Rml::String& tag) : Rml::Element(tag), view_(SkinView::current()) {
    if (view_) view_->registerElement(this);
}
PButtonElement::~PButtonElement() {
    if (view_) {
        if (down_) view_->button(raw_, false);
        view_->unregisterElement(this);
    }
}

void PButtonElement::OnAttributeChange(const Rml::ElementAttributes& changed) {
    Rml::Element::OnAttributeChange(changed);
    if (changed.count("raw")) raw_ = parseInt(attr(*this, "raw"), -1);
    if (changed.count("led")) led_ = parseInt(attr(*this, "led"), -1);
}

void PButtonElement::mouseDown(bool alt) {
    if (!view_ || raw_ < 0 || raw_ > 0x7F) return;
    if (view_->isHeld(raw_)) { view_->setHeld(raw_, false); return; }   // any click on a held button releases it
    if (alt) { view_->setHeld(raw_, true); return; }
    down_ = true;
    SetClass("pressed", true);
    view_->setMousePressed(this);
    view_->button(raw_, true);
}

void PButtonElement::mouseRelease() {
    if (!down_) return;
    down_ = false;
    SetClass("pressed", false);
    if (view_) view_->button(raw_, false);
}

// ------------------------------------------------------------------ LED and display digit

LedElement::LedElement(const Rml::String& tag) : Rml::Element(tag), view_(SkinView::current()) {
    if (view_) view_->registerElement(this);
}
LedElement::~LedElement() { if (view_) view_->unregisterElement(this); }

void LedElement::OnAttributeChange(const Rml::ElementAttributes& changed) {
    Rml::Element::OnAttributeChange(changed);
    if (changed.count("code")) code_ = parseInt(attr(*this, "code"), -1);
    if (changed.count("source")) audioClip_ = attr(*this, "source") == "audioclip";
}

VfDigitElement::VfDigitElement(const Rml::String& tag) : SpriteElement(tag), view_(SkinView::current()) {
    if (view_) view_->registerElement(this);
}
VfDigitElement::~VfDigitElement() { if (view_) view_->unregisterElement(this); }

void VfDigitElement::OnAttributeChange(const Rml::ElementAttributes& changed) {
    SpriteElement::OnAttributeChange(changed);
    if (changed.count("pos")) {
        pos_ = std::clamp(parseInt(attr(*this, "pos"), 0), 0, 3);
        if (view_) setByte(view_->displayBytes()[size_t(pos_)]);
    }
}

}  // namespace skin
