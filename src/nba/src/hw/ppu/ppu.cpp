/*
 * Copyright (C) 2021 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#include <cstring>

#include "hw/ppu/ppu.hpp"

namespace nba::core {

PPU::PPU(
  Scheduler& scheduler,
  IRQ& irq,
  DMA& dma,
  std::shared_ptr<Config> config
)   : scheduler(scheduler)
    , irq(irq)
    , dma(dma)
    , config(config) {
  mmio.dispcnt.ppu = this;
  mmio.dispstat.ppu = this;
  Reset();
}

void PPU::Reset() {
  std::memset(pram, 0, 0x00400);
  std::memset(oam,  0, 0x00400);
  std::memset(vram, 0, 0x18000);

  mmio.dispcnt.Reset();
  mmio.dispstat.Reset();

  for (int i = 0; i < 4; i++) {
    enable_bg[0][i] = false;
    enable_bg[1][i] = false;

    mmio.bgcnt[i].Reset();
    mmio.bghofs[i] = 0;
    mmio.bgvofs[i] = 0;

    if (i < 2) {
      mmio.bgx[i].Reset();
      mmio.bgy[i].Reset();
      mmio.bgpa[i] = 0x100;
      mmio.bgpb[i] = 0;
      mmio.bgpc[i] = 0;
      mmio.bgpd[i] = 0x100;
    }
  }

  mmio.winh[0].Reset();
  mmio.winh[1].Reset();
  mmio.winv[0].Reset();
  mmio.winv[1].Reset();
  mmio.winin.Reset();
  mmio.winout.Reset();

  mmio.mosaic.Reset();

  mmio.eva = 0;
  mmio.evb = 0;
  mmio.evy = 0;
  mmio.bldcnt.Reset();

   // VCOUNT=225 DISPSTAT=3 was measured after reset on a 3DS in GBA mode (thanks Lady Starbreeze).
  mmio.vcount = 225;
  mmio.dispstat.vblank_flag = true;
  mmio.dispstat.hblank_flag = true;
  scheduler.Add(226, this, &PPU::OnVblankHblankComplete);
}

void PPU::LatchEnabledBGs() {
  for (int i = 0; i < 4; i++) {
    enable_bg[0][i] = enable_bg[1][i];
  }

  for (int i = 0; i < 4; i++) {
    enable_bg[1][i] = mmio.dispcnt.enable[i];
  }
}

void PPU::CheckVerticalCounterIRQ() {
  auto& dispstat = mmio.dispstat;
  auto vcount_flag_new = dispstat.vcount_setting == mmio.vcount;

  if (dispstat.vcount_irq_enable && !dispstat.vcount_flag && vcount_flag_new) {
    irq.Raise(IRQ::Source::VCount);
  }
  
  dispstat.vcount_flag = vcount_flag_new;
}

void PPU::OnScanlineComplete(int cycles_late) {
  auto& bgx = mmio.bgx;
  auto& bgy = mmio.bgy;
  auto& bgpb = mmio.bgpb;
  auto& bgpd = mmio.bgpd;
  auto& mosaic = mmio.mosaic;

  scheduler.Add(226 - cycles_late, this, &PPU::OnHblankComplete);

  UpdateScanline();

  mmio.dispstat.hblank_flag = 1;

  if (mmio.dispstat.hblank_irq_enable) {
    irq.Raise(IRQ::Source::HBlank);
  }

  dma.Request(DMA::Occasion::HBlank);
  
  if (mmio.vcount >= 2) {
    dma.Request(DMA::Occasion::Video);
  }

  // Advance vertical background mosaic counter
  if (++mosaic.bg._counter_y == mosaic.bg.size_y) {
    mosaic.bg._counter_y = 0;
  }

  // Advance vertical OBJ mosaic counter
  if (++mosaic.obj._counter_y == mosaic.obj.size_y) {
    mosaic.obj._counter_y = 0;
  }

  /* Mode 0 doesn't have any affine backgrounds,
   * in that case the internal X/Y registers will never be updated.
   */
  if (mmio.dispcnt.mode != 0) {
    // Advance internal affine registers and apply vertical mosaic if applicable.
    for (int i = 0; i < 2; i++) {
      /* Do not update internal X/Y unless the latched BG enable bit is set.
       * This behavior was confirmed on real hardware.
       */
      auto bg_id = 2 + i;

      if (enable_bg[0][bg_id]) {
        if (mmio.bgcnt[bg_id].mosaic_enable) {
          if (mosaic.bg._counter_y == 0) {
            bgx[i]._current += mosaic.bg.size_y * bgpb[i];
            bgy[i]._current += mosaic.bg.size_y * bgpd[i];
          }
        } else {
          bgx[i]._current += bgpb[i];
          bgy[i]._current += bgpd[i];
        }
      }
    }
  }

  /* TODO: it appears that this should really happen ~36 cycles into H-draw.
   * But right now if we do that it breaks at least Pinball Tycoon.
   */
  LatchEnabledBGs();
}

void PPU::OnHblankComplete(int cycles_late) {
  auto& dispcnt = mmio.dispcnt;
  auto& dispstat = mmio.dispstat;
  auto& vcount = mmio.vcount;
  auto& bgx = mmio.bgx;
  auto& bgy = mmio.bgy;
  auto& mosaic = mmio.mosaic;

  dispstat.hblank_flag = 0;
  vcount++;
  CheckVerticalCounterIRQ();

  if (dispcnt.enable[ENABLE_WIN0]) {
    RenderWindow(0);
  }

  if (dispcnt.enable[ENABLE_WIN1]) {
    RenderWindow(1);
  }

  if (vcount == 160) {
    config->video_dev->Draw(output);

    scheduler.Add(1006 - cycles_late, this, &PPU::OnVblankScanlineComplete);
    dma.Request(DMA::Occasion::VBlank);
    dispstat.vblank_flag = 1;

    if (dispstat.vblank_irq_enable) {
      irq.Raise(IRQ::Source::VBlank);
    }

    // Reset vertical mosaic counters
    mosaic.bg._counter_y = 0;
    mosaic.obj._counter_y = 0;

    // Reload internal affine registers
    bgx[0]._current = bgx[0].initial;
    bgy[0]._current = bgy[0].initial;
    bgx[1]._current = bgx[1].initial;
    bgy[1]._current = bgy[1].initial;
  } else {
    scheduler.Add(1006 - cycles_late, this, &PPU::OnScanlineComplete);
    BeginScanline();
    // Render OBJs for the next scanline.
    if (mmio.dispcnt.enable[ENABLE_OBJ]) {
      RenderLayerOAM(mmio.dispcnt.mode >= 3, mmio.vcount + 1);
    }
  }
}

void PPU::OnVblankScanlineComplete(int cycles_late) {
  auto& dispstat = mmio.dispstat;

  scheduler.Add(226 - cycles_late, this, &PPU::OnVblankHblankComplete);

  dispstat.hblank_flag = 1;

  if (mmio.vcount < 162) {
    dma.Request(DMA::Occasion::Video);
  } else if (mmio.vcount == 162) {
    dma.StopVideoXferDMA();
  }

  if (dispstat.hblank_irq_enable) {
    irq.Raise(IRQ::Source::HBlank);
  }

  if (mmio.vcount >= 225) {
    /* TODO: it appears that this should really happen ~36 cycles into H-draw.
     * But right now if we do that it breaks at least Pinball Tycoon.
     */
    LatchEnabledBGs();
  }
}

void PPU::OnVblankHblankComplete(int cycles_late) {
  auto& vcount = mmio.vcount;
  auto& dispstat = mmio.dispstat;

  dispstat.hblank_flag = 0;

  if (vcount == 227) {
    scheduler.Add(1006 - cycles_late, this, &PPU::OnScanlineComplete);
    vcount = 0;
  } else {
    scheduler.Add(1006 - cycles_late, this, &PPU::OnVblankScanlineComplete);
    if (++vcount == 227) {
      dispstat.vblank_flag = 0;
      // Render OBJs for the next scanline
      if (mmio.dispcnt.enable[ENABLE_OBJ]) {
        RenderLayerOAM(mmio.dispcnt.mode >= 3, 0);
      }
    }
  }

  if (mmio.dispcnt.enable[ENABLE_WIN0]) {
    RenderWindow(0);
  }

  if (mmio.dispcnt.enable[ENABLE_WIN1]) {
    RenderWindow(1);
  }

  if (vcount == 0) {
    BeginScanline();
    // Render OBJs for the next scanline
    if (mmio.dispcnt.enable[ENABLE_OBJ]) {
      RenderLayerOAM(mmio.dispcnt.mode >= 3, 1);
    }
  }

  CheckVerticalCounterIRQ();
}

void PPU::BeginScanline() {
  renderer.time = 0;

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

  renderer.timestamp = scheduler.GetTimestampNow();
}

void PPU::UpdateScanline() {
  if (mmio.dispstat.hblank_flag || mmio.dispstat.vblank_flag) {
    return;
  }

  auto mode = mmio.dispcnt.mode;
  auto cycles = int(scheduler.GetTimestampNow() - renderer.timestamp);

  switch (mode) {
    case 0:
      UpdateScanlineMode0(cycles);
      break;
    case 1:
      UpdateScanlineMode1(cycles);
      break;
    case 2:
      UpdateScanlineMode2(cycles);
      break;
    case 3:
      UpdateScanlineMode3(cycles);
      break;
    case 4:
      UpdateScanlineMode4(cycles);
      break;
    case 5:
      UpdateScanlineMode5(cycles);
      break;
    case 6:
    case 7:
      UpdateScanlineMode67(cycles);
      break;
  }

  renderer.timestamp = scheduler.GetTimestampNow();
}

// TODO: transparent pixels don't need to write to the BG buffer.

void PPU::UpdateTextLayer(int id, int cycle) {
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

void PPU::UpdateAffineLayer(int id, int cycle) {
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

void PPU::UpdateScanlineMode0(int cycles) {
  while (cycles-- > 0) {
    auto id = (renderer.time & 31) >> 3;
    
    if (renderer.bg[id].engaged) {
      auto cycle = renderer.time & 7;
      UpdateTextLayer(id, cycle);
    }

    // TODO: implement this in a better way.
    // TODO: perform horizontal mosaic operation.
    if (++renderer.time == 1006) {
      ComposeScanline(0, 3);
    }
  }
}

void PPU::UpdateScanlineMode1(int cycles) {
  while (cycles-- > 0) {
    auto id = (renderer.time & 31) >> 3;

    if (id <= 1) {
      if (renderer.bg[id].engaged) {
        auto cycle = renderer.time & 7;
        UpdateTextLayer(id, cycle);
      }
    } else {
      if (renderer.bg[2].engaged) {
        auto cycle = renderer.time & 1;
        UpdateAffineLayer(2, cycle);
      }
    }

    // TODO: implement this in a better way.
    // TODO: perform horizontal mosaic operation.
    if (++renderer.time == 1006) {
      ComposeScanline(0, 3);
    }
  }
}

void PPU::UpdateScanlineMode2(int cycles) {
  while (cycles-- > 0) {
    auto id = 2 + ((renderer.time >> 4) & 1);

    if (renderer.bg[id].engaged) {
      auto cycle = renderer.time & 1;
      UpdateAffineLayer(id, cycle);
    }
    
    // TODO: implement this in a better way.
    // TODO: perform horizontal mosaic operation.
    if (++renderer.time == 1006) {
      ComposeScanline(0, 3);
    }
  }
}

void PPU::UpdateScanlineMode3(int cycles) {
  // TODO: write a test ROM to figure out the actual timing.
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

    // TODO: implement this in a better way.
    // TODO: perform horizontal mosaic operation.
    if (++renderer.time == 1006) {
      ComposeScanline(0, 3);
    }
  }
}

void PPU::UpdateScanlineMode4(int cycles) {
  // TODO: write a test ROM to figure out the actual timing.
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

    // TODO: implement this in a better way.
    // TODO: perform horizontal mosaic operation.
    if (++renderer.time == 1006) {
      ComposeScanline(0, 3);
    }
  }
}

void PPU::UpdateScanlineMode5(int cycles) {
  // TODO: write a test ROM to figure out the actual timing.
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

    // TODO: implement this in a better way.
    // TODO: perform horizontal mosaic operation.
    if (++renderer.time == 1006) {
      ComposeScanline(0, 3);
    }
  }
}

void PPU::UpdateScanlineMode67(int cycles) {
}

} // namespace nba::core
