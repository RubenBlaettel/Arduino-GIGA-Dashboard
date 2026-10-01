#include <mbed.h>
#include <stdio.h>
#include "platform/mbed_error.h"
#include "diag.h"

static const uint32_t MAGIC_FAULT = 0xFA170000u;
static const uint32_t MAGIC_HANG = 0x4A560000u;
static const uint32_t MAGIC_MASK = 0xFFFF0000u;

static volatile uint16_t curPhase = PH_SETUP;
static volatile uint32_t beat = 0;
static char lastReset[96] = "";
static rtos::Thread wdThread(osPriorityRealtime, 1024, nullptr, "diag-wd");

// Survives a system reset: the core's linker script keeps .pdm_buffer (SRAM4, 0x3800FC00) as
// NOLOAD, so startup code does not clear it. The PDM microphone library is not used here.
// (RTC backup registers did not keep the note, although the RTC itself runs on through a reset.)
struct ResetNote { uint32_t mark, status, pc, lr; };
static volatile ResetNote note __attribute__((section(".pdm_buffer")));

// Called by mbed for every fatal error, before it would halt and blink the LED.
extern "C" void mbed_error_hook(const mbed_error_ctx *ctx) {
  uint32_t pc = 0, lr = 0;
  uint32_t v = ctx->error_value;
  if ((v & 0xFF000000u) == 0x24000000u || (v & 0xFF000000u) == 0x20000000u) {
    const uint32_t *fault = (const uint32_t *)v;  // mbed_fault_context_t: R0..R12, SP, LR, PC, ...
    lr = fault[14];
    pc = fault[15];
  }
  note.status = (uint32_t)ctx->error_status;
  note.pc = pc;
  note.lr = lr;
  note.mark = MAGIC_FAULT | curPhase;
  SCB_CleanDCache();  // the note must reach RAM before the reset
  __DSB();
  NVIC_SystemReset();
}

static void watchdog_loop() {
  uint32_t last = beat;
  int stalled = 0;
  for (;;) {
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(500));
    if (beat != last) {
      last = beat;
      stalled = 0;
    } else if (++stalled >= 16) {  // 8 s without a loop() pass
      note.mark = MAGIC_HANG | curPhase;
      SCB_CleanDCache();
      __DSB();
      NVIC_SystemReset();
    }
  }
}

void diag_begin() {
  uint32_t mark = note.mark;
  if ((mark & MAGIC_MASK) == MAGIC_FAULT) {
    snprintf(lastReset, sizeof(lastReset), "fault status=0x%08lX pc=0x%08lX lr=0x%08lX phase=%u",
             (unsigned long)note.status, (unsigned long)note.pc, (unsigned long)note.lr,
             (unsigned)(mark & 0xFFFF));
  } else if ((mark & MAGIC_MASK) == MAGIC_HANG) {
    snprintf(lastReset, sizeof(lastReset), "hang phase=%u", (unsigned)(mark & 0xFFFF));
  }
  note.mark = 0;
  wdThread.start(watchdog_loop);
}

void diag_phase(uint16_t phase) {
  curPhase = phase;
  beat = beat + 1;
}

const char *diag_last_reset() { return lastReset; }
