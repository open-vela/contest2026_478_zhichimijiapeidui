#include <nuttx/config.h>
#include <nuttx/audio/audio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <nuttx/sched.h>

#define FRAME_BYTES 640
#define SAMPLE_RATE 16000
#define TONE_HZ 2000

extern int board_audio_i2s_initialize(void);
extern int board_audio_i2s_stop(void);
extern void board_audio_i2s_set_repeat(bool enable);
extern int board_audio_codec_hw_tone(int seconds);
extern int board_audio_play_tone(int seconds);
extern int board_audio_stream_start(void);
extern int board_audio_stream_read(void *buf, size_t len);
extern int board_audio_stream_wait(int timeout_ms);
extern int board_audio_stream_stop(void);
extern uint32_t board_audio_stream_dropped(void);
extern int board_audio_play_start(void);
extern int board_audio_play_write(const void *buf, size_t len);
extern int board_audio_play_drain(int timeout_ms);
extern int board_audio_play_stop(void);
extern uint32_t board_audio_play_dropped(void);
extern uint32_t board_audio_play_underrun(void);
static volatile bool g_test_running;
static int g_test_seconds;
static bool g_test_loop;
static bool g_test_tone;
static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int tone(int seconds);
static int mic(int seconds, int loop);

static int audio_test_worker(int argc, FAR char *argv[])
{
  int ret;
  (void)argc;
  (void)argv;
  ret = g_test_tone ? tone(g_test_seconds) : mic(g_test_seconds, g_test_loop);
  g_test_running = false;
  return ret;
}

static int configure(int fd)
{
  struct audio_caps_desc_s caps;
  memset(&caps, 0, sizeof(caps));
  caps.caps.ac_len = sizeof(caps.caps);
  caps.caps.ac_type = AUDIO_TYPE_OUTPUT | AUDIO_TYPE_INPUT;
  caps.caps.ac_controls.w = SAMPLE_RATE;
  caps.caps.ac_channels = 1;
  caps.caps.ac_format.hw = AUDIO_FMT_PCM;
  return ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&caps);
}

static int tone(int seconds)
{
  int fd = open("/dev/i2schar0", O_RDWR);
  int16_t samples[FRAME_BYTES / 2];
  int i, ret;
  if (fd < 0) { printf("audio: open failed %d\n", errno); return 1; }
  ret = configure(fd);
  if (ret < 0) { printf("audio: configure failed %d\n", ret); close(fd); return 1; }
  /* 2 kHz sine at 16 kHz.  Keep several complete cycles in every DMA
   * buffer so the speaker receives a steady, clearly audible test tone. */
  for (i = 0; i < FRAME_BYTES / 2; i++)
    samples[i] = (int16_t)(14000.0 * sin(6.283185307 * TONE_HZ * i /
                                         SAMPLE_RATE));
  {
      struct audio_buf_desc_s desc;
      struct ap_buffer_s *apb = NULL;

      /* ap_buffer_s contains an internal reference/mutex state.  Allocate
       * it through the audio buffer pool; a zeroed heap object is invalid
       * and trips apb_reference() inside i2schar_write(). */
      memset(&desc, 0, sizeof(desc));
      desc.numbytes = FRAME_BYTES;
      desc.u.pbuffer = &apb;
      if (apb_alloc(&desc) <= 0 || apb == NULL)
        {
          printf("audio: tone buffer allocation failed\n");
          close(fd);
          return 1;
        }

      apb->nbytes = FRAME_BYTES;
      memcpy(apb->samp, samples, FRAME_BYTES);
      board_audio_i2s_set_repeat(true);
      ret = write(fd, apb, sizeof(*apb) + FRAME_BYTES);
      if (ret < 0)
        {
          printf("audio: tone write failed err=%d\n", errno);
          board_audio_i2s_set_repeat(false);
          apb_free(apb);
          close(fd);
          return 1;
        }

      /* HAL configures AUDCODEC DMA as circular.  Keep this one frame
       * repeating for the requested duration, then stop it explicitly. */
      usleep((useconds_t)seconds * 1000000U);
      board_audio_i2s_set_repeat(false);
      board_audio_i2s_stop();
      /* write() added a transfer reference; stop callback releases it. */
      apb_free(apb);
    }
  close(fd); printf("audio: tone complete\n"); return 0;
}

static int mic(int seconds, int loop)
{
  int fd = open("/dev/i2schar0", O_RDWR | O_NONBLOCK);
  int frames = seconds * SAMPLE_RATE / (FRAME_BYTES / 2);
  int f, ret, nonzero = 0;
  if (fd < 0) { printf("audio: open failed %d\n", errno); return 1; }
  ret = configure(fd);
  if (ret < 0) { printf("audio: configure failed %d\n", ret); close(fd); return 1; }
  for (f = 0; f < frames; f++)
    {
      struct audio_buf_desc_s desc;
      struct ap_buffer_s *apb = NULL;

      memset(&desc, 0, sizeof(desc));
      desc.numbytes = FRAME_BYTES;
      desc.u.pbuffer = &apb;
      if (apb_alloc(&desc) <= 0 || apb == NULL)
        {
          printf("audio: mic buffer allocation failed frame=%d\n", f);
          close(fd);
          return 1;
        }

      ret = read(fd, apb, sizeof(*apb) + apb->nmaxbytes);
      if (ret >= (int)sizeof(struct ap_buffer_s))
        {
          /* i2schar_read starts DMA asynchronously and returns immediately.
           * Let one 16-kHz frame complete before inspecting the buffer. */
          usleep(25000);
          int16_t *p = (int16_t *)apb->samp;
          int i;
          int peak = 0;
          for (i = 0; i < FRAME_BYTES / 2; i++) if (abs(p[i]) > peak) peak = abs(p[i]);
          printf("audio: mic frame=%d peak=%d\n", f, peak); nonzero |= peak != 0;
          if (loop)
            {
              apb->nbytes = FRAME_BYTES;
              ret = write(fd, apb, sizeof(*apb) + FRAME_BYTES);
              if (ret < 0)
                printf("audio: loop write failed frame=%d err=%d\n", f, errno);
            }
        }
      else if (errno != EAGAIN && errno != EINTR) printf("audio: read failed %d\n", errno);
      /* Keep the application's reference.  The I2S callback owns and drops
       * the transfer reference when DMA completes. */
      apb_free(apb);
      if (ret < 0 && errno != EAGAIN && errno != EINTR)
        {
          close(fd);
          return 1;
        }
      if (ret < (int)sizeof(struct ap_buffer_s)) usleep(20000);
    }
  close(fd); printf("audio: mic complete, nonzero=%d\n", nonzero); return 0;
}

int main(int argc, char **argv)
{
  int seconds = argc > 2 ? atoi(argv[2]) : 5;
  if (argc >= 2 && strcmp(argv[1], "status") == 0)
    {
      /* Counters tell which side of the link is the bottleneck: uplink drops
       * mean the consumer is too slow, downlink underruns mean frames are not
       * arriving fast enough. */
      printf("audio: stream_dropped=%u play_dropped=%u play_underrun=%u\n",
             (unsigned)board_audio_stream_dropped(),
             (unsigned)board_audio_play_dropped(),
             (unsigned)board_audio_play_underrun());
      return 0;
    }
  if (argc < 2)
    { printf("usage: sf32_audio_test tone [sec] | stream [sec] | loopback [sec] | mic [sec] | loop [sec] | hw [sec] | status\n"); return 0; }
  if (strcmp(argv[1], "hw") == 0)
    {
      printf("audio: hardware 1k test started\n");
      return board_audio_codec_hw_tone(seconds);
    }
  if (strcmp(argv[1], "tone") == 0)
    {
      printf("audio: direct tone started\n");
      return board_audio_play_tone(seconds);
    }
  if (strcmp(argv[1], "loopback") == 0)
    {
      int16_t frame[320];
      int frames = 0;
      int starved = 0;
      int peak = 0;
      int worst = 0;
      /* Microphone and speaker sit centimetres apart on the DevKit, so
       * feeding capture straight back to playback closes an acoustic loop
       * and howls once the loop gain exceeds one.  Attenuate by default;
       * argv[3] overrides the shift (0 = no attenuation). */
      int shift = argc > 3 ? atoi(argv[3]) : 2;

      if (shift < 0 || shift > 8)
        {
          shift = 2;
        }

      if (board_audio_play_start() < 0)
        {
          printf("audio: loopback play start failed\n");
          return 1;
        }
      if (board_audio_stream_start() < 0)
        {
          printf("audio: loopback capture start failed\n");
          board_audio_play_stop();
          return 1;
        }
      printf("audio: loopback started shift=%d, speak into the microphone\n",
             shift);

      /* Capture frames and hand them straight to playback.  No UART, no
       * framing: this isolates the audio driver from the link. */
      while (frames < seconds * 50 && starved < 100)
        {
          if (board_audio_stream_wait(200) < 0)
            {
              starved++;
              continue;
            }
          while (board_audio_stream_read(frame, sizeof(frame)) ==
                 (int)sizeof(frame))
            {
              int i;

              for (i = 0; i < 320; i++)
                {
                  int v = frame[i] < 0 ? -frame[i] : frame[i];

                  if (v > peak)
                    {
                      peak = v;
                    }
                }
              if (peak > worst)
                {
                  worst = peak;
                }
              if (shift > 0)
                {
                  for (i = 0; i < 320; i++)
                    {
                      frame[i] = (int16_t)(frame[i] >> shift);
                    }
                }
              board_audio_play_write(frame, sizeof(frame));
              frames++;
            }
        }

      board_audio_stream_stop();
      board_audio_play_drain(1000);
      board_audio_play_stop();
      printf("audio: loopback done frames=%d peak=%d starved=%d drop_up=%u "
             "drop_dn=%u under=%u\n", frames, worst, starved,
             (unsigned)board_audio_stream_dropped(),
             (unsigned)board_audio_play_dropped(),
             (unsigned)board_audio_play_underrun());
      return 0;
    }  if (strcmp(argv[1], "stream") == 0)
    {
      int16_t frame[320];
      int frames = 0;
      int empty = 0;
      int peak = 0;
      int ret;

      /* P4: "slow" delays each read past the 20 ms frame period so the
       * 8-frame queue overflows on purpose.  Production must keep the frame
       * rate and drop instead of blocking. */
      int slow_ms = (argc > 3 && strcmp(argv[3], "slow") == 0) ? 60 : 0;
      uint32_t t0;
      uint32_t elapsed_ms;

      if (board_audio_stream_start() < 0)
        {
          printf("audio: stream start failed\n");
          return 1;
        }
      printf("audio: stream started, %d frames expected%s\n", seconds * 50,
             slow_ms ? " (slow consumer)" : "");
      t0 = now_ms();

      /* Block on the frame semaphore instead of polling.  The system tick is
       * 20 ms, the same as one frame, so a usleep loop burns a wake-up per
       * frame and leaves no slack for UART work. */
      /* Slow mode drops frames, so frames alone never reaches the target;
       * bound that run by wall clock instead. */
      while (empty < 50 &&
             (slow_ms ? (now_ms() - t0) < (uint32_t)seconds * 1000U
                      : frames < seconds * 50))
        {
          int wret = board_audio_stream_wait(200);

          if (wret < 0)
            {
              empty++;
              continue;
            }
          ret = board_audio_stream_read(frame, sizeof(frame));
          if (ret == (int)sizeof(frame))
            {
              int i;

              for (i = 0; i < 320; i++)
                {
                  int v = frame[i] < 0 ? -frame[i] : frame[i];

                  if (v > peak)
                    {
                      peak = v;
                    }
                }
              frames++;
              if (slow_ms)
                {
                  usleep(slow_ms * 1000);
                }
              if ((frames % 50) == 0)
                {
                  printf("audio: stream %d frames peak=%d dropped=%u\n",
                         frames, peak, (unsigned)board_audio_stream_dropped());
                  peak = 0;
                }
            }
          else
            {
              empty++;
            }
        }

      /* Drain what the DMA queued while the loop was exiting, otherwise the
       * tail frames are missing from produced and the measured rate reads
       * low.  In slow mode the final usleep() alone hides ~3 frames. */
      while (board_audio_stream_read(frame, sizeof(frame)) ==
             (int)sizeof(frame))
        {
          frames++;
        }
      elapsed_ms = now_ms() - t0;
      board_audio_stream_stop();
      {
        uint32_t produced = (uint32_t)frames +
                            board_audio_stream_dropped();
        uint32_t rate_x100 = elapsed_ms ?
                             produced * 100000U / elapsed_ms : 0;

        printf("audio: stream done frames=%d empty=%d dropped=%u\n",
               frames, empty, (unsigned)board_audio_stream_dropped());
        printf("audio: P1 elapsed=%ums produced=%u rate=%u.%02u fps "
               "(target 50.00, tol 2%%)\n",
               (unsigned)elapsed_ms, (unsigned)produced,
               (unsigned)(rate_x100 / 100), (unsigned)(rate_x100 % 100));
        if (rate_x100 >= 4900 && rate_x100 <= 5100)
          {
            printf("audio: PASS P1 frame rate within tolerance\n");
          }
        else
          {
            printf("audio: FAIL P1 frame rate out of tolerance\n");
          }
        if (slow_ms)
          {
            if (board_audio_stream_dropped() > 0)
              {
                printf("audio: PASS P4 queue dropped without stalling\n");
              }
            else
              {
                printf("audio: FAIL P4 expected drops under slow consumer\n");
              }
          }
      }
      return 0;
    }
  if (g_test_running)
    { printf("audio: test already running\n"); return 1; }
  if (board_audio_i2s_initialize() < 0)
    { printf("audio: device registration failed\n"); return 1; }
  g_test_running = true;
  g_test_seconds = seconds;
  if (strcmp(argv[1], "mic") == 0 || strcmp(argv[1], "loop") == 0)
    {
      int pid;
      g_test_tone = false;
      g_test_loop = strcmp(argv[1], "loop") == 0;
      pid = task_create("audio_mic", 100, 4096, audio_test_worker, NULL);
      if (pid < 0) g_test_running = false;
      printf("audio: %s started (pid=%d)\n", argv[1], pid); return pid < 0;
    }
  printf("usage: sf32_audio_test tone [sec] | stream [sec] | loopback [sec] | mic [sec] | loop [sec] | hw [sec] | status\n"); return 1;
}
