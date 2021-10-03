/*
 * Copyright (C) 2021 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#include <algorithm>

#include "hw/ppu/ppu.hpp"

namespace nba::core {

using BlendMode = BlendControl::Effect;

void PPU::BeginComposer(int cycles_late) {
  composer.engaged = true;
  composer.time = 0;
  composer.timestamp = scheduler.GetTimestampNow();

  // TODO: the mode is probably latched earlier in the scanline.
  switch (mmio.dispcnt.mode) {
    case 0: {
      composer.bg_min = 0;
      composer.bg_max = 3;
      break;
    }
    case 1: {
      composer.bg_min = 0;
      composer.bg_max = 2;
      break;
    }
    case 2: {
      composer.bg_min = 2;
      composer.bg_max = 3;
      break;
    }
    default: {
      composer.bg_min = 2;
      composer.bg_max = 2;
      break;
    }
  }
}

void PPU::Compose(int cycles) {
  auto const& dispcnt = mmio.dispcnt;
  auto const& bgcnt = mmio.bgcnt;
  auto const& winin = mmio.winin;
  auto const& winout = mmio.winout;

  int bg_list[4];
  int bg_count = 0;

  int bg_min = composer.bg_min;
  int bg_max = composer.bg_max;

  // Sort enabled backgrounds by their respective priority in ascending order.
  for (int prio = 3; prio >= 0; prio--) {
    for (int bg = bg_max; bg >= bg_min; bg--) {
      if (enable_bg[0][bg] && dispcnt.enable[bg] && bgcnt[bg].priority == prio) {
        bg_list[bg_count++] = bg;
      }
    }
  }

  u32* line = &output[mmio.vcount * 240];

  // TODO: the DISPCNT enable bits probably are affected by the two scanline delay as well...
  bool window = dispcnt.enable[ENABLE_WIN0] ||
                dispcnt.enable[ENABLE_WIN1] ||
                dispcnt.enable[ENABLE_OBJWIN];
  bool win0_active = dispcnt.enable[ENABLE_WIN0] && window_scanline_enable[0];
  bool win1_active = dispcnt.enable[ENABLE_WIN1] && window_scanline_enable[1];
  bool win2_active = dispcnt.enable[ENABLE_OBJWIN];

  int prio[2];
  int layer[2];
  u16 pixel[2];

  int const* win_layer_enable;

  while (cycles-- > 0) {
    auto cycle = composer.time & 3;

    // TODO: the composer can't always finish in a single cycle.. .
    if (cycle == 0) {
      auto x = composer.time >> 2;

      if (window) {
        // Determine the window with the highest priority for this pixel.
        // TODO: think about possible optimisations here.
        if (win0_active && buffer_win[0][x]) {
          win_layer_enable = winin.enable[0];
        } else if (win1_active && buffer_win[1][x]) {
          win_layer_enable = winin.enable[1];
        } else if (win2_active && buffer_obj[x].window) {
          win_layer_enable = winout.enable[1];
        } else {
          win_layer_enable = winout.enable[0];
        }
      }

      bool is_alpha_obj = false;

      prio[0] = 4;
      prio[1] = 4;
      layer[0] = LAYER_BD;
      layer[1] = LAYER_BD;
      
      // Find up to two top-most visible background pixels.
      for (int i = 0; i < bg_count; i++) {
        int bg = bg_list[i];

        if (!window || win_layer_enable[bg]) {
          auto pixel_new = buffer_bg[bg][x];
          if (pixel_new != s_color_transparent) {
            layer[1] = layer[0];
            layer[0] = bg;
            prio[1] = prio[0];
            prio[0] = bgcnt[bg].priority;
          }
        }
      }

      /* Check if a OBJ pixel takes priority over one of the two
       * top-most background pixels and insert it accordingly.
       */
      if ((!window || win_layer_enable[LAYER_OBJ]) &&
          dispcnt.enable[ENABLE_OBJ] &&
          buffer_obj[x].color != s_color_transparent) {
        int priority = buffer_obj[x].priority;

        if (priority <= prio[0]) {
          layer[1] = layer[0];
          layer[0] = LAYER_OBJ;
          is_alpha_obj = buffer_obj[x].alpha;
        } else if (priority <= prio[1]) {
          layer[1] = LAYER_OBJ;
        }
      }

      // Map layer numbers to pixels.
      for (int i = 0; i < 2; i++) {
        int _layer = layer[i];
        switch (_layer) {
          case 0 ... 3:
            pixel[i] = buffer_bg[_layer][x];
            break;
          case 4:
            pixel[i] = buffer_obj[x].color;
            break;
          case 5:
            pixel[i] = read<u16>(pram, 0);
            break;
        }
      }

      if (!window || win_layer_enable[LAYER_SFX] || is_alpha_obj) {
        auto blend_mode = mmio.bldcnt.sfx;
        bool have_dst = mmio.bldcnt.targets[0][layer[0]];
        bool have_src = mmio.bldcnt.targets[1][layer[1]];

        if (is_alpha_obj && have_src) {
          Blend(pixel[0], pixel[1], BlendMode::SFX_BLEND);
        } else if (have_dst && blend_mode != BlendMode::SFX_NONE && (have_src || blend_mode != BlendMode::SFX_BLEND)) {
          Blend(pixel[0], pixel[1], blend_mode);
        }
      }

      line[x] = ConvertColor(pixel[0]);
    }

    if (++composer.time == 960) {
      composer.engaged = false;
      break;
    }
  }
}

void PPU::Blend(
  u16& target1,
  u16  target2,
  BlendMode sfx
) {
  int r1 = (target1 >>  0) & 0x1F;
  int g1 = (target1 >>  5) & 0x1F;
  int b1 = (target1 >> 10) & 0x1F;

  switch (sfx) {
    case BlendMode::SFX_BLEND: {
      int eva = std::min<int>(16, mmio.eva);
      int evb = std::min<int>(16, mmio.evb);

      int r2 = (target2 >>  0) & 0x1F;
      int g2 = (target2 >>  5) & 0x1F;
      int b2 = (target2 >> 10) & 0x1F;

      r1 = std::min<u8>((r1 * eva + r2 * evb) >> 4, 31);
      g1 = std::min<u8>((g1 * eva + g2 * evb) >> 4, 31);
      b1 = std::min<u8>((b1 * eva + b2 * evb) >> 4, 31);
      break;
    }
    case BlendMode::SFX_BRIGHTEN: {
      int evy = std::min<int>(16, mmio.evy);
      
      r1 += ((31 - r1) * evy) >> 4;
      g1 += ((31 - g1) * evy) >> 4;
      b1 += ((31 - b1) * evy) >> 4;
      break;
    }
    case BlendMode::SFX_DARKEN: {
      int evy = std::min<int>(16, mmio.evy);
      
      r1 -= (r1 * evy) >> 4;
      g1 -= (g1 * evy) >> 4;
      b1 -= (b1 * evy) >> 4;
      break;
    }
  }

  target1 = r1 | (g1 << 5) | (b1 << 10);
}

auto PPU::ConvertColor(u16 color) -> u32 {
  int r = (color >>  0) & 0x1F;
  int g = (color >>  5) & 0x1F;
  int b = (color >> 10) & 0x1F;

  return r << 19 |
         g << 11 |
         b <<  3 |
         0xFF000000;
}

} // namespace nba::core
