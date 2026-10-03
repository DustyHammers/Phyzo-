// The custom skin elements: <knob cc>, <pbutton raw led>, <led code|source>, <vfdigit pos>.
#pragma once
#include <RmlUi/Core/Element.h>
#include <string>

namespace skin {

class SkinView;

void registerElements();          // once per RmlUi initialisation

// Shows sprite <spriteprefix><NNN> (three digits) through the element's decorator.
class SpriteElement : public Rml::Element {
public:
    explicit SpriteElement(const Rml::String& tag) : Rml::Element(tag) {}
    const std::string& shownSprite() const { return shown_; }   // "image(<sprite>)" or empty
protected:
    void showSprite(int index);
    std::string prefix();
private:
    std::string shown_;
};

// Value = raw 0-1023; frame = round(raw x (frames-1) / 1023). Sends Bx cc vv only when the user (or Lua) moves it.
class KnobElement : public SpriteElement {
public:
    explicit KnobElement(const Rml::String& tag);
    ~KnobElement() override;
    int cc() const { return cc_; }
    int raw() const { return raw_; }
    // Moves the knob to raw: stores its position and sends the value (unless unchanged).
    void move(int raw);
    // Shows raw and stores it, without sending (the value was sent already, e.g. by panel.setKnob).
    void show(int raw);
    void wheel(float notches, bool fine);
protected:
    void OnUpdate() override;
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    std::string key() const;
    void redraw();
    SkinView* view_;
    int cc_ = -1;
    int raw_ = 0;
    bool known_ = false;
};

// Left button: 81 raw while down, 80 raw on release (class pressed). Alt-click: latching hold (class held).
class PButtonElement : public Rml::Element {
public:
    explicit PButtonElement(const Rml::String& tag);
    ~PButtonElement() override;
    int raw() const { return raw_; }
    int ledCode() const { return led_; }
    void mouseDown(bool alt);
    void mouseRelease();             // the left button went up after mouseDown
protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    SkinView* view_;
    int raw_ = -1, led_ = -1;
    bool down_ = false;
};

// Class lit per the LED logic; source="audioclip" is the Input Clip LED.
class LedElement : public Rml::Element {
public:
    explicit LedElement(const Rml::String& tag);
    ~LedElement() override;
    int code() const { return code_; }
    bool audioClip() const { return audioClip_; }
protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    SkinView* view_;
    int code_ = -1;
    bool audioClip_ = false;
};

// One display digit: pos 0 = leftmost (panel message 96) ... 3 = rightmost (93). Sprite = segment byte & 0x7F.
class VfDigitElement : public SpriteElement {
public:
    explicit VfDigitElement(const Rml::String& tag);
    ~VfDigitElement() override;
    int pos() const { return pos_; }
    void setByte(uint8_t b) { byte_ = b & 0x7F; showSprite(byte_); }
protected:
    void OnUpdate() override { showSprite(byte_); }
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    SkinView* view_;
    int pos_ = 0;
    int byte_ = 0;
};

int parseInt(const std::string& s, int fallback);   // decimal or 0x hex

}  // namespace skin
