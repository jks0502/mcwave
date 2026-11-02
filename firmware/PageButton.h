#pragma once
#include <stdint.h>

enum class ButtonAction { None, Switch, Next };
class PageButton {
  bool raw_ = false, pressed_ = false, held_ = false;
  uint32_t changed_ = 0, down_ = 0;
 public:
  ButtonAction update(bool down, uint32_t now) {
    if (down != raw_) { raw_ = down; changed_ = now; }
    if (raw_ != pressed_ && uint32_t(now-changed_) >= 40) {
      pressed_ = raw_;
      if (pressed_) { down_ = now; held_ = false; }
      else if (!held_) return ButtonAction::Switch;
    }
    if (pressed_ && !held_ && uint32_t(now-down_) >= 1000) {
      held_ = true;
      return ButtonAction::Next;
    }
    return ButtonAction::None;
  }
};
