/* SPDX-License-Identifier: MIT */
/* Real generated driver and prompt writer, with RAM-backed hardware registers. */
#define PLATFORM_HRC7000_H
#define HRC_DMR_GLUE_H
#define pdFALSE 0
#define portYIELD_FROM_ISR(x) ((void)(x))
#define IRQ_PCM_WR 28
#include <assert.h>
#include <stdio.h>
#include "modem/hrc7000/hrc7000.h"
#include "FreeRTOS.h"
static uint32_t now_us;
static uint32_t hrc_micros(void) { return now_us; }
static uint32_t hrc_irq_save(void) { return 0; }
static void hrc_irq_restore(uint32_t s) { (void)s; }
static void hrc_irq_enable(unsigned irq,bool on) { (void)irq;(void)on; }
static void hrc_irq_register(unsigned irq,void (*fn)(void)) { (void)irq;(void)fn; }
static void hrc_barrier(void) { }
static bool hrc_dmr_tx_active(void) { return false; }
static bool tone_active;
bool beep_busy(void) {return tone_active;}
void hrc7000_set_crit(hrc7000_t *d,uint32_t (*a)(void),void (*b)(uint32_t)) { d->crit_enter=a;d->crit_exit=b; }
void hrc7000_audio_bits(hrc7000_t *d,uint32_t set,uint32_t clear) { d->audio_ctl=(d->audio_ctl&~clear)|set; }
void vTaskNotifyGiveFromISR(TaskHandle_t t,BaseType_t *w) { (void)t;(void)w; }
BaseType_t xTaskCreate(void (*fn)(void *),const char *n,unsigned s,void *c,unsigned p,TaskHandle_t *h)
{ (void)fn;(void)n;(void)s;(void)c;(void)p;*h=(void *)1;return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t c,TickType_t t) { (void)c;(void)t;return 0; }
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
#include "../../build/radiofoundry-p25/platform/hrc7000/audio_pcm.c"
/* Actual board ownership logic; keep modem/source hardware local to the test. */
#define AMP_RADIO 1u
#define AMP_PCM 2u
#define PIN_AMP_EN 10u
static bool pcm_on, amp_enabled;
static unsigned amp_writes, source_changes;
static hrc7000_t *board_modem;
static void hrc_pin_write(unsigned pin,bool on)
{ assert(pin==PIN_AMP_EN);amp_enabled=on;amp_writes++; }
static void speaker_source(void) { source_changes++; }
static void ring_play(void) { hrc_audio_pcm_ring(board_modem,pcm_on); }
#define modem (*board_modem)
#include "../../build/audio-host/board_speaker.inc"
#undef modem
static void enqueue(unsigned serial)
{
    uint8_t next=(uint8_t)((spk_head+1u)%SPK_BLOCKS);assert(next!=spk_tail);
    for(unsigned i=0;i<160;i++)spk_q[spk_head][i]=(int16_t)(serial*160+i);
    spk_loud[spk_head]=true;spk_head=next;
}
int main(void)
{
    uint32_t regs[1024]={0},diag[16];uint8_t buf[4096]={0};
    hrc7000_t chip={.regs=regs,.buf=buf};board_modem=&chip;hrc_audio_pcm_attach(&chip,dm32_speaker_pcm);
    speaker_pcm(true);playback_pace(true);
    for(now_us=0;now_us<1000000;now_us+=2000) {
        if(now_us%20000==0)enqueue(now_us/20000);
        fifo_feed();fifo_feed(); /* before/after callback cannot burst */
    }
    playback_stats(diag);
    assert(diag[2]==250 && diag[5]==4000 && diag[6]==4000 && !diag[7] && !diag[10]);
    assert(!play_clock.late);
    enqueue(50);regs[C7_AUDIO_CONTROL/4]=C7_AUDIO_RING_FULL;
    unsigned pos=spk_pos;fifo_feed();assert(spk_pos==pos && play_diag[3]==1);
    regs[C7_AUDIO_CONTROL/4]=0;fifo_feed();assert(spk_pos==32);
    /* Exact little-endian samples, with no skipped chunk on a full refusal. */
    assert(buf[C7_PROMPT_FIFO]==(8000&255) && buf[C7_PROMPT_FIFO+1]==(8000>>8));
    fifo_held=true;now_us+=4000;fifo_feed();assert(spk_pos==32);fifo_held=false;
    tx_gated=true;fifo_feed();assert(spk_pos==32);tx_gated=false;
    fifo_feed();assert(spk_pos==64);
    now_us+=40000;fifo_feed();assert(play_clock.late==1);
    unsigned n=play_diag[2];fifo_feed();assert(play_diag[2]==n);
    spk_drop();playback_pace(false);enqueue(0);enqueue(1);fifo_feed();
    playback_stats(diag);assert(diag[2]==10 && diag[7]==9 && !diag[10]);
    /* Automatic muting leaves routing/queues intact and waits for the actual
     * final loud chunk's bounded FIFO tail. Idle polls cause no GPIO writes. */
    spk_drop();loud_recent=false;playback_pace(true);speaker_pcm(true);
    assert(pcm_on && spk_routed && !amp_enabled && !hrc_audio_pcm_amp_wanted());
    unsigned writes=amp_writes, sources=source_changes, generation=spk_gen;
    for(unsigned i=0;i<100;i++){now_us+=2000;fifo_feed();}
    assert(amp_writes==writes && source_changes==sources && spk_gen==generation);
#if CONFIG_BEEP
    tone_active=true;fifo_feed();assert(amp_enabled&&!hrc_audio_pcm_spk_pending());
    writes=amp_writes;for(unsigned i=0;i<100;i++){now_us+=2000;fifo_feed();}
    assert(amp_enabled&&amp_writes==writes); /* tone lead/gap does not click the amplifier */
    tone_active=false;fifo_feed();assert(!amp_enabled);
#endif
    enqueue(2);speaker_pcm(true);
    assert(amp_enabled && spk_gen==generation && spk_head!=spk_tail);
    for(unsigned i=0;i<5;i++){now_us+=4000;fifo_feed();assert(amp_enabled);}
    assert(spk_head==spk_tail);
    now_us=loud_pushed+31999;fifo_feed();assert(amp_enabled);
    now_us++;fifo_feed();assert(!amp_enabled && pcm_on && spk_routed && spk_gen==generation);
    writes=amp_writes;
    /* Silence still clocks through the FIFO but cannot wake the amplifier. */
    memset(spk_q[spk_head],0,sizeof spk_q[spk_head]);spk_loud[spk_head]=false;
    spk_head=(uint8_t)((spk_head+1u)%SPK_BLOCKS);
    for(unsigned i=0;i<5;i++){now_us+=4000;fifo_feed();}
    assert(!amp_enabled && amp_writes==writes && spk_head==spk_tail);
    enqueue(3);now_us+=4000;fifo_feed();assert(amp_enabled);
    fifo_held=true;unsigned before=spk_pos;fifo_feed();assert(!amp_enabled && spk_pos==before);
    fifo_held=false;now_us+=4000;fifo_feed();assert(amp_enabled);
    tx_gated=true;before=spk_pos;fifo_feed();assert(!amp_enabled && spk_pos==before);
    tx_gated=false;now_us+=4000;fifo_feed();assert(amp_enabled);
    /* FM owns the same physical amplifier independently of paced PCM. */
    amp(AMP_RADIO,true);chip.audio_ctl|=C7_AUDIO_FM_PLAY;
    spk_drop();loud_recent=false;generation=spk_gen;sources=source_changes;
    fifo_feed();assert(amp_enabled && !hrc_audio_pcm_amp_wanted());
    assert((chip.audio_ctl&C7_AUDIO_FM_PLAY) && pcm_on && spk_routed);
    assert(spk_gen==generation && source_changes==sources);
    amp(AMP_RADIO,false);assert(!amp_enabled);
    /* FIFO-tail subtraction remains correct across the 32-bit clock wrap. */
    playback_pace(true);now_us=UINT32_MAX-10000u;enqueue(4);fifo_feed();assert(amp_enabled);
    spk_drop();now_us=loud_pushed+31999u;fifo_feed();assert(amp_enabled);
    now_us++;fifo_feed();assert(!amp_enabled);
    playback_pace(false);assert(amp_enabled); /* legacy beeps keep their route */
    speaker_pcm(false);assert(!amp_enabled && !pcm_on && !spk_routed);
    /* Ownership gates must silence the amp synchronously, before a task tick. */
    spk_drop();loud_recent=false;playback_pace(true);speaker_pcm(true);enqueue(5);fifo_feed();assert(amp_enabled);
    hrc_audio_pcm_hold(true);assert(!amp_enabled);
    hrc_audio_pcm_hold(false);assert(amp_enabled);
    hrc_audio_pcm_tx_gate();assert(!amp_enabled && tx_gated && spk_head==spk_tail);
    now_us+=GATE_MIN_US;gate_check();fifo_feed();assert(!tx_gated && !amp_enabled);
    puts("Real PCM driver: continuous 8 kHz, full retry/sample order, hold/TX gates, missed deadline, legacy A/B PASS");
}
