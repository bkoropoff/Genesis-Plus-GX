/***************************************************************************************
 *  Genesis Plus
 *  Sound Hardware
 *
 *  Copyright (C) 1998-2003  Charles Mac Donald (original code)
 *  Copyright (C) 2007-2016  Eke-Eke (Genesis Plus GX)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************/

#include "shared.h"
#include "blip_buf.h"

/* FM output buffer (large enough to hold a whole frame at original chips rate) */
#ifdef HAVE_YM3438_CORE
static int fm_buffer[1080 * 2 * 24];
#else
static int fm_buffer[1080 * 2];
#endif

static int fm_last[2];
static int *fm_ptr;

/* Cycle-accurate FM samples */
static uint32 fm_cycles_ratio;
static uint32 fm_cycles_start;
static uint32 fm_cycles_count;

/* YM chip function pointers */
static void (*YM_Reset)(void);
static void (*YM_Update)(int *buffer, int length);
static void (*YM_Write)(unsigned int a, unsigned int v);
static unsigned int (*YM_Read)(unsigned int a);

#ifdef HAVE_YM3438_CORE
static ym3438_t ym3438;
static int ym3438_accm[24][2];
static int ym3438_sample[2];
static unsigned int ym3438_cycles;

void YM3438_Reset(void)
{
  OPN2_Reset(&ym3438);
}

void YM3438_Update(int *buffer, int length)
{
  int i, j;
  for (i = 0; i < length; i++)
  {
    OPN2_Clock(&ym3438, ym3438_accm[ym3438_cycles]);
    ym3438_cycles = (ym3438_cycles + 1) % 24;
    if (ym3438_cycles == 0)
    {
      ym3438_sample[0] = 0;
      ym3438_sample[1] = 0;
      for (j = 0; j < 24; j++)
      {
        ym3438_sample[0] += ym3438_accm[j][0];
        ym3438_sample[1] += ym3438_accm[j][1];
      }
    }
    *buffer++ = ym3438_sample[0] * 11;
    *buffer++ = ym3438_sample[1] * 11;
  }
}

void YM3438_Write(unsigned int a, unsigned int v)
{
  OPN2_Write(&ym3438, a, v);
}

unsigned int YM3438_Read(unsigned int a)
{
  return OPN2_Read(&ym3438, a);
}
#endif

/* Run FM chip until required M-cycles */
INLINE void fm_update(unsigned int cycles)
{
  if (cycles > fm_cycles_count)
  {
    /* number of samples to run */
    unsigned int samples = (cycles - fm_cycles_count + fm_cycles_ratio - 1) / fm_cycles_ratio;

    /* run FM chip to sample buffer */
    YM_Update(fm_ptr, samples);

    /* update FM buffer pointer */
    fm_ptr += (samples << 1);

    /* update FM cycle counter */
    fm_cycles_count += samples * fm_cycles_ratio;
  }
}

void sound_init( void )
{
  /* Initialize FM chip */
  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
  {
    /* YM2612 */
    #ifdef HAVE_YM3438_CORE
    if (config.ym3438)
    {
      /* Nuked OPN2 */
      memset(&ym3438, 0, sizeof(ym3438));
      memset(&ym3438_sample, 0, sizeof(ym3438_sample));
      memset(&ym3438_accm, 0, sizeof(ym3438_accm));
      YM_Reset = YM3438_Reset;
      YM_Update = YM3438_Update;
      YM_Write = YM3438_Write;
      YM_Read = YM3438_Read;

      /* chip is running at VCLK / 6 = MCLK / 7 / 6 */
      fm_cycles_ratio = 6 * 7;
    }
    else
    #endif
    {
      /* MAME */
      YM2612Init();
      YM2612Config(config.dac_bits);
      YM_Reset = YM2612ResetChip;
      YM_Update = YM2612Update;
      YM_Write = YM2612Write;
      YM_Read = YM2612Read;

      /* chip is running at VCLK / 144 = MCLK / 7 / 144 */
      fm_cycles_ratio = 144 * 7;
    }
  }
  else
  {
    /* YM2413 */
    YM2413Init();
    YM_Reset = YM2413ResetChip;
    YM_Update = YM2413Update;
    YM_Write = YM2413Write;
    YM_Read = NULL;

    /* chip is running at ZCLK / 72 = MCLK / 15 / 72 */
    fm_cycles_ratio = 72 * 15;
  }

  /* Initialize PSG chip */
  psg_init((system_hw == SYSTEM_SG) ? PSG_DISCRETE : PSG_INTEGRATED);
}

void sound_reset(void)
{
  /* reset sound chips */
  YM_Reset();
  psg_reset();
  psg_config(0, config.psg_preamp, 0xff);

  /* reset FM buffer ouput */
  fm_last[0] = fm_last[1] = 0;

  /* reset FM buffer pointer */
  fm_ptr = fm_buffer;
  
  /* reset FM cycle counters */
  fm_cycles_start = fm_cycles_count = 0;
}

int sound_update(unsigned int cycles)
{
  int prev_l, prev_r, preamp, time, l, r, *ptr;

  /* Run PSG chip until end of frame */
  psg_end_frame(cycles);

  /* Run FM chip until end of frame */
  fm_update(cycles);

  /* FM output pre-amplification */
  preamp = config.fm_preamp;

  /* FM frame initial timestamp */
  time = fm_cycles_start;

  /* Restore last FM outputs from previous frame */
  prev_l = fm_last[0];
  prev_r = fm_last[1];

  /* FM buffer start pointer */
  ptr = fm_buffer;

  /* flush FM samples */
  if (config.hq_fm)
  {
    /* high-quality Band-Limited synthesis */
    do
    {
      /* left & right channels */
      l = ((*ptr++ * preamp) / 100);
      r = ((*ptr++ * preamp) / 100);
      blip_add_delta(snd.blips[0], time, l-prev_l, r-prev_r);
      prev_l = l;
      prev_r = r;

      /* increment time counter */
      time += fm_cycles_ratio;
    }
    while (time < cycles);
  }
  else
  {
    /* faster Linear Interpolation */
    do
    {
      /* left & right channels */
      l = ((*ptr++ * preamp) / 100);
      r = ((*ptr++ * preamp) / 100);
      blip_add_delta_fast(snd.blips[0], time, l-prev_l, r-prev_r);
      prev_l = l;
      prev_r = r;

      /* increment time counter */
      time += fm_cycles_ratio;
    }
    while (time < cycles);
  }

  /* reset FM buffer pointer */
  fm_ptr = fm_buffer;

  /* save last FM output for next frame */
  fm_last[0] = prev_l;
  fm_last[1] = prev_r;

  /* adjust FM cycle counters for next frame */
  fm_cycles_count = fm_cycles_start = time - cycles;

  /* end of blip buffer time frame */
  blip_end_frame(snd.blips[0], cycles);

  /* return number of available samples */
  return blip_samples_avail(snd.blips[0]);
}

int sound_context_save(uint8 *state)
{
  int bufferptr = 0;
  
  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
  {
    #ifdef HAVE_YM3438_CORE
    save_param(&config.ym3438, sizeof(config.ym3438));
    if (config.ym3438)
    {
      save_param(&ym3438, sizeof(ym3438));
      save_param(&ym3438_accm, sizeof(ym3438_accm));
      save_param(&ym3438_sample, sizeof(ym3438_sample));
      save_param(&ym3438_cycles, sizeof(ym3438_cycles));
    }
    else
    {
      bufferptr += YM2612SaveContext(state + sizeof(config.ym3438));
      YM2612Config(config.dac_bits);
    }
    #else
    bufferptr = YM2612SaveContext(state);
    #endif
  }
  else
  {
    save_param(YM2413GetContextPtr(),YM2413GetContextSize());
  }

  bufferptr += psg_context_save(&state[bufferptr]);

  save_param(&fm_cycles_start,sizeof(fm_cycles_start));

  return bufferptr;
}

int sound_context_load(uint8 *state)
{
  int bufferptr = 0;
  uint8 config_ym3438;

  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
  {
    #ifdef HAVE_YM3438_CORE
    load_param(&config_ym3438, sizeof(config_ym3438));
    if (config_ym3438)
    {
      load_param(&ym3438, sizeof(ym3438));
      load_param(&ym3438_accm, sizeof(ym3438_accm));
      load_param(&ym3438_sample, sizeof(ym3438_sample));
      load_param(&ym3438_cycles, sizeof(ym3438_cycles));
    }
    else
    {
        bufferptr += YM2612LoadContext(state + sizeof(config_ym3438));
        YM2612Config(config.dac_bits);
    }
    #else
    bufferptr = YM2612LoadContext(state);
    YM2612Config(config.dac_bits);
    #endif
  }
  else
  {
    load_param(YM2413GetContextPtr(),YM2413GetContextSize());
  }

  bufferptr += psg_context_load(&state[bufferptr]);

  load_param(&fm_cycles_start,sizeof(fm_cycles_start));
  fm_cycles_count = fm_cycles_start;

  return bufferptr;
}

void fm_reset(unsigned int cycles)
{
  /* synchronize FM chip with CPU */
  fm_update(cycles);

  /* reset FM chip */
  YM_Reset();
}

void fm_write(unsigned int cycles, unsigned int address, unsigned int data)
{
  /* synchronize FM chip with CPU */
  fm_update(cycles);
  
  /* write FM register */
  YM_Write(address, data);
}

unsigned int fm_read(unsigned int cycles, unsigned int address)
{
  /* synchronize FM chip with CPU */
  fm_update(cycles);

  /* read FM status (YM2612 only) */
  return YM_Read(address);
}
