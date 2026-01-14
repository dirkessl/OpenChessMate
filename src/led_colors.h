#ifndef LED_COLORS_H
#define LED_COLORS_H

#include <stdint.h>

struct LedRGB {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

namespace LedColors {
// Consistent colors across Chess Bot + Chess Moves modes
static constexpr LedRGB PickupCyan{0, 255, 255};       // cyan
static constexpr LedRGB MoveWhite{255, 255, 255};      // normal move
static constexpr LedRGB AttackRed{255, 0, 0};          // capture/attack
static constexpr LedRGB ConfirmGreen{0, 255, 0};       // move completion
static constexpr LedRGB CheckAmber{255, 140, 0};       // king in check
static constexpr LedRGB ErrorRed{255, 0, 0};           // error/invalid move
static constexpr LedRGB Gold{255, 215, 0};             // promotion/special
static constexpr LedRGB BotThinkingWhite{0, 255, 255}; // bot thinking (white)
static constexpr LedRGB BotThinkingBlack{0, 0, 255};   // bot thinking (black)
static constexpr LedRGB Off{0, 0, 0};                  // turn off LED
} // namespace LedColors

#endif // LED_COLORS_H
