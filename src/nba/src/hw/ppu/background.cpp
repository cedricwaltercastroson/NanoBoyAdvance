/*
 * Copyright (C) 2021 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#include "hw/ppu/ppu.hpp"

namespace nba::core {

void PPU::BeginRenderBG() {
  renderer.time = 0;
  renderer.timestamp = scheduler.GetTimestampNow();

  // TODO: only do what is necessary for the current mode.
  for (int id = 0; id < 4; id++) {
    auto& bg = renderer.bg[id];

    bg.engaged = enable_bg[0][id];

    if (bg.engaged) {
      // Text mode
      bg.grid_x = 0;
      bg.draw_x = -(mmio.bghofs[id] & 7);

      // Affine modes
      if (id >= 2) {
        if (mmio.dispcnt.mode != 0) {
          bg.draw_x = 0; // ugh
        }
        bg.ref_x = mmio.bgx[id & 1]._current;
        bg.ref_y = mmio.bgy[id & 1]._current;
      }

      for (int x = 0; x < 240; x++) {
        buffer_bg[id][x] = s_color_transparent;
      }
    }
  }
}

void PPU::RenderLayerText(int id, int cycle) {
  auto& bg = renderer.bg[id];

  /*
   * Access patterns (current theory):
   *
   *   8BPP:
   *    #0 - fetch map
   *    #1 - fetch pixels #0 - #1 (16-bit)
   *    #2 - fetch pixels #2 - #3 (16-bit)
   *    #3 - fetch pixels #4 - #5 (16-bit)
   *    #4 - fetch pixels #6 - #7 (16-bit)
   *    #N - idle
   *
   *   4BPP:
   *    #0 - fetch map 
   *    #1 - fetch pixels #0 - #3 (16-bit)
   *    #2 - idle
   *    #3 - fetch pixels #4 - #7 (16-bit)
   *    #4 - idle
   *    #N - idle
   */

  if (cycle == 0) {
    bg.enabled = mmio.dispcnt.enable[id];

    if (bg.enabled) {
      auto& bgcnt  = mmio.bgcnt[id];
      auto tile_base = bgcnt.tile_block << 14;
      auto map_block = bgcnt.map_block;

      auto line = mmio.bgvofs[id] + mmio.vcount;
  
      auto grid_x = (mmio.bghofs[id] >> 3) + bg.grid_x;
      auto grid_y = line >> 3;
      auto tile_y = line & 7;

      auto screen_x = (grid_x >> 5) & 1;
      auto screen_y = (grid_y >> 5) & 1;

      switch (bgcnt.size) {
        case 1:
          map_block += screen_x;
          break;
        case 2:
          map_block += screen_y;
          break;
        case 3:
          map_block += screen_x;
          map_block += screen_y << 1;
          break;
      }

      auto address = (map_block << 11) + ((grid_y & 31) << 6) + ((grid_x & 31) << 1);

      auto map_entry = read<u16>(vram, address);
      auto number = map_entry & 0x3FF;
      auto palette = map_entry >> 12;
      bool flip_x = map_entry & (1 << 10);
      bool flip_y = map_entry & (1 << 11);

      if (flip_y) tile_y ^= 7;

      bg.palette = (u16*)&pram[palette << 5];
      bg.full_palette = bgcnt.full_palette;
      bg.flip_x = flip_x;

      if (bgcnt.full_palette) {
        bg.address = tile_base + (number << 6) + (tile_y << 3);
        if (flip_x) {
          bg.address += 6;
        }
      } else {
        bg.address = tile_base + (number << 5) + (tile_y << 2);
        if (flip_x) {
          bg.address += 2;
        }
      }
    }
  } else if (cycle <= 4) {
    auto& address = bg.address;

    if (bg.full_palette) {
      if (bg.enabled && mmio.dispcnt.enable[id]) {        
        auto data = read<u16>(vram, address);
        auto flip = bg.flip_x ? 1 : 0;
        auto draw_x = bg.draw_x;
        auto palette = (u16*)pram;

        for (int x = 0; x < 2; x++) {
          u16 color;
          u8 index = u8(data);

          if (index == 0) {
            color = s_color_transparent;
          } else {
            color = palette[index];
          }

          // TODO: optimise this!!!
          // Solution: make BG buffer bigger to allow for overflow on each side!
          auto final_x = draw_x + (x ^ flip);
          if (final_x >= 0 && final_x <= 239) {
            buffer_bg[id][final_x] = color;
          }

          data >>= 8;
        }
      }
      
      bg.draw_x += 2;
      
      // TODO: test if the address is updated if the BG is disabled.
      if (bg.flip_x) {
        address -= sizeof(u16);
      } else {
        address += sizeof(u16);
      }
    } else if (cycle & 1) {
      // TODO: it could be that the four pixels are output over multiple (two?) cycles.
      // In that case it wouldn't be sufficient to output all pixels at once.
      if (bg.enabled && mmio.dispcnt.enable[id]) {
        auto data = read<u16>(vram, address);
        auto flip = bg.flip_x ? 3 : 0;
        auto draw_x = bg.draw_x;
        auto palette = bg.palette;

        for (int x = 0; x < 4; x++) {
          u16 color;
          u16 index = data & 15;

          if (index == 0) {
            color = s_color_transparent;
          } else {
            color = palette[index];
          }

          // TODO: optimise this!!!
          // Solution: make BG buffer bigger to allow for overflow on each side!
          auto final_x = draw_x + (x ^ flip);
          if (final_x >= 0 && final_x <= 239) {
            buffer_bg[id][final_x] = color;
          }

          data >>= 4;
        }
      }
      
      bg.draw_x += 4;
      
      // TODO: test if the address is updated if the BG is disabled.
      if (bg.flip_x) {
        address -= sizeof(u16);
      } else {
        address += sizeof(u16);
      }
    }

    if (cycle == 4) {
      // TODO: should we stop at 30 if (BGHOFS & 7) == 0?
      if (++bg.grid_x == 31) {
        bg.engaged = false;
      }
    }
  }
}

void PPU::RenderLayerAffine(int id, int cycle) {
  auto& bg = renderer.bg[id];
  auto& bgcnt = mmio.bgcnt[id];
  auto& pa = mmio.bgpa[id & 1];
  auto& pc = mmio.bgpc[id & 1];

  /*
   * Access pattern (current theory):
   *
   *   4BPP/8BPP:
   *    # 0 - fetch map 
   *    # 1 - fetch single pixel
   *    # 2 - fetch map
   *    # 3 - fetch single pixel
   *    # 4 - fetch map
   *    # 5 - fetch single pixel
   *    # 6 - fetch map
   *    # 7 - fetch single pixel
   *    # 8 - fetch map 
   *    # 9 - fetch single pixel
   *    #10 - fetch map
   *    #11 - fetch single pixel
   *    #12 - fetch map
   *    #13 - fetch single pixel
   *    #14 - fetch map
   *    #15 - fetch single pixel
   */

  if (cycle == 0) {
    bg.enabled = mmio.dispcnt.enable[id];

    if (bg.enabled) {
      auto x = bg.ref_x >> 8;
      auto y = bg.ref_y >> 8;

      bg.ref_x += pa;
      bg.ref_y += pc;

      auto size = 128 << bgcnt.size;
      auto mask = size - 1;

      if (bgcnt.wraparound) {
        x &= mask;
        y &= mask;
      } else {
        // disable if either X or Y is outside the [0, size) range.
        bg.enabled = ((x | y) & -size) == 0;
      }

      if (bg.enabled) {
        auto map_base  = bgcnt.map_block << 11;
        auto tile_base = bgcnt.tile_block << 14;
        auto number = vram[map_base + ((y >> 3) << (4 + bgcnt.size)) + (x >> 3)];

        bg.address = tile_base + (number << 6) + ((y & 7) << 3) + (x & 7);
      }
    }      
  } else {
    u16 color = s_color_transparent;

    if (bg.enabled && mmio.dispcnt.enable[id]) {
      u8 index = vram[bg.address];
      if (index != 0) {
        color = read<u16>(pram, index << 1);
      }
    }

    buffer_bg[id][bg.draw_x] = color;

    if (++bg.draw_x == 240) {
      bg.engaged = false;
    }
  }
}

void PPU::RenderMode0(int cycles) {
  while (cycles-- > 0) {
    auto id = (renderer.time & 31) >> 3;
    
    if (renderer.bg[id].engaged) {
      auto cycle = renderer.time & 7;
      RenderLayerText(id, cycle);
    }

    renderer.time++;
  }
}

void PPU::RenderMode1(int cycles) {
  while (cycles-- > 0) {
    auto id = (renderer.time & 31) >> 3;

    if (id <= 1) {
      if (renderer.bg[id].engaged) {
        auto cycle = renderer.time & 7;
        RenderLayerText(id, cycle);
      }
    } else {
      if (renderer.bg[2].engaged) {
        auto cycle = renderer.time & 1;
        RenderLayerAffine(2, cycle);
      }
    }

    renderer.time++;
  }
}

void PPU::RenderMode2(int cycles) {
  while (cycles-- > 0) {
    auto id = 2 + ((renderer.time >> 4) & 1);

    if (renderer.bg[id].engaged) {
      auto cycle = renderer.time & 1;
      RenderLayerAffine(id, cycle);
    }

    renderer.time++;
  }
}

void PPU::RenderMode3(int cycles) {
  while (cycles-- > 0) {
    auto cycle = renderer.time & 31;

    if (cycle < 16) {
      auto& bg = renderer.bg[2];

      if (bg.engaged && mmio.dispcnt.enable[2]) {
        auto x = u32(bg.ref_x >> 8);
        auto y = u32(bg.ref_y >> 8);

        bg.ref_x += mmio.bgpa[0];
        bg.ref_y += mmio.bgpc[0];

        if (x < 240 && y < 160) {
          auto index = (y * 240 + x) << 1;
          buffer_bg[2][bg.draw_x] = read<u16>(vram, index);
        }

        if (++bg.draw_x == 240) {
          bg.engaged = false;
        }
      }
    }

    renderer.time++;
  }
}

void PPU::RenderMode4(int cycles) {
  while (cycles-- > 0) {
    auto cycle = renderer.time & 31;

    if (cycle < 16) {
      auto& bg = renderer.bg[2];

      if (bg.engaged && mmio.dispcnt.enable[2]) {
        auto x = u32(bg.ref_x >> 8);
        auto y = u32(bg.ref_y >> 8);

        bg.ref_x += mmio.bgpa[0];
        bg.ref_y += mmio.bgpc[0];

        if (x < 240 && y < 160) {
          auto frame = mmio.dispcnt.frame * 0xA000;
          auto index = vram[frame + y * 240 + x] << 1;
          if (index != 0) {
            buffer_bg[2][bg.draw_x] = read<u16>(pram, index);
          }
        }

        if (++bg.draw_x == 240) {
          bg.engaged = false;
        }
      }
    }

    renderer.time++;
  }
}

void PPU::RenderMode5(int cycles) {
  while (cycles-- > 0) {
    auto cycle = renderer.time & 31;

    if (cycle < 16) {
      auto& bg = renderer.bg[2];

      if (bg.engaged && mmio.dispcnt.enable[2]) {
        auto x = u32(bg.ref_x >> 8);
        auto y = u32(bg.ref_y >> 8);

        bg.ref_x += mmio.bgpa[0];
        bg.ref_y += mmio.bgpc[0];

        if (x < 160 && y < 128) {
          auto frame = mmio.dispcnt.frame * 0xA000;
          auto index = frame + (y * 160 + x) << 1;
          if (index != 0) {
            buffer_bg[2][bg.draw_x] = read<u16>(pram, index);
          }
        }

        if (++bg.draw_x == 240) {
          bg.engaged = false;
        }
      }
    }

    renderer.time++;
  }
}

void PPU::RenderMode67(int cycles) {
}

} // namespace nba::core
