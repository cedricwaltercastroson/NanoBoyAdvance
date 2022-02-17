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
  scheduler.Add(224, this, &PPU::OnVblankHblankComplete);
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

  scheduler.Add(224 - cycles_late, this, &PPU::OnHblankComplete);

  mmio.dispstat.hblank_flag = 1;

  scheduler.Add(2, [this](int late) {
    if (mmio.dispstat.hblank_irq_enable) {
      irq.Raise(IRQ::Source::HBlank);
    }
  });

  /*if (mmio.dispstat.hblank_irq_enable) {
    irq.Raise(IRQ::Source::HBlank);
  }*/

  //dma.Request(DMA::Occasion::HBlank);

  // TODO: time Video DMA correctly too!!!
  if (mmio.vcount >= 2) {
    dma.Request(DMA::Occasion::Video);
  }

  // Advance vertical background mosaic counter
  if (++mosaic.bg._counter_y == mosaic.bg.size_y) {
    mosaic.bg._counter_y = 0;
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

    scheduler.Add(1008 - cycles_late, this, &PPU::OnVblankScanlineComplete);
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
    scheduler.Add(1006 - cycles_late, [this](int late) {
      dma.Request(DMA::Occasion::HBlank);
    });

    scheduler.Add(1008 - cycles_late, this, &PPU::OnScanlineComplete);
    RenderScanline();
    // Render OBJs for the next scanline.
    if (mmio.dispcnt.enable[ENABLE_OBJ]) {
      RenderLayerOAM(mmio.dispcnt.mode >= 3, mmio.vcount + 1);
    }
  }
}

void PPU::OnVblankScanlineComplete(int cycles_late) {
  auto& dispstat = mmio.dispstat;

  scheduler.Add(224 - cycles_late, this, &PPU::OnVblankHblankComplete);

  dispstat.hblank_flag = 1;

  if (mmio.vcount < 162) {
    dma.Request(DMA::Occasion::Video);
  } else if (mmio.vcount == 162) {
    dma.StopVideoXferDMA();
  }

  scheduler.Add(2, [this](int late) {
    if (mmio.dispstat.hblank_irq_enable) {
      irq.Raise(IRQ::Source::HBlank);
    }
  });

  /*if (dispstat.hblank_irq_enable) {
    irq.Raise(IRQ::Source::HBlank);
  }*/

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
    scheduler.Add(1006 - cycles_late, [this](int late) {
      dma.Request(DMA::Occasion::HBlank);
    });

    scheduler.Add(1008 - cycles_late, this, &PPU::OnScanlineComplete);
    vcount = 0;
  } else {
    scheduler.Add(1008 - cycles_late, this, &PPU::OnVblankScanlineComplete);
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
    RenderScanline();
    // Render OBJs for the next scanline
    if (mmio.dispcnt.enable[ENABLE_OBJ]) {
      RenderLayerOAM(mmio.dispcnt.mode >= 3, 1);
    }
  }

  CheckVerticalCounterIRQ();
}

} // namespace nba::core
