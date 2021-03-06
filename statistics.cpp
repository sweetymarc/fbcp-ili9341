#include "config.h"

#include "statistics.h"

#ifdef STATISTICS

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <pthread.h>
#include <syslog.h>

#include "tick.h"
#include "text.h"
#include "spi.h"
#include "util.h"

volatile uint64_t timeWastedPollingGPU = 0;
volatile int statsSpiBusSpeed = 0;
volatile int statsCpuFrequency = 0;
volatile double statsCpuTemperature = 0;
double spiThreadUtilizationRate;
double spiBusDataRate;
int statsGpuPollingWasted = 0;
uint64_t statsBytesTransferred = 0;

int frameSkipTimeHistorySize = 0;
uint64_t frameSkipTimeHistory[FRAME_HISTORY_MAX_SIZE] = {};

char fpsText[32] = {};
char spiUsagePercentageText[32] = {};
char spiBusDataRateText[32] = {};
uint16_t spiUsageColor = 0, fpsColor = 0;
char statsFrameSkipText[32] = {};
char spiSpeedText[32] = {};
char cpuTemperatureText[32] = {};
uint16_t cpuTemperatureColor = 0;
char gpuPollingWastedText[32] = {};
uint16_t gpuPollingWastedColor = 0;

uint64_t statsLastPrint = 0;

void *poll_thread(void *unused)
{
  for(;;)
  {
    usleep(1000000);
    // SPI bus speed
    FILE *handle = popen("vcgencmd measure_clock core", "r");
    char t[64] = {};
    if (handle)
    {
      int ret = fread(t, 1, sizeof(t)-1, handle);
      pclose(handle);
    }
    char *s = t;
    while(*s && *s != '=') ++s;
    if (*s == '=') ++s;
    statsSpiBusSpeed = atoi(s)/1000000;

    // CPU temperature
    handle = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    char t2[32] = {};
    if (handle)
    {
      fread(t2, 1, sizeof(t2)-1, handle);
      fclose(handle);
    }
    statsCpuTemperature = atoi(t2)/1000.0;

    // Raspberry pi main CPU core speed
    handle = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    char t3[32] = {};
    if (handle)
    {
      fread(t3, 1, sizeof(t3)-1, handle);
      fclose(handle);
    }
    statsCpuFrequency = atoi(t3) / 1000;
  }
}

int InitStatistics()
{
  pthread_t thread;
  int rc = pthread_create(&thread, NULL, poll_thread, NULL);
  if (rc != 0) FATAL_ERROR("Failed to create Statistics polling thread!");
}

void DrawStatisticsOverlay(uint16_t *framebuffer)
{
  DrawText(framebuffer, gpuFrameWidth, gpuFramebufferScanlineStrideBytes, gpuFrameHeight, fpsText, 1, 1, fpsColor, 0);
  DrawText(framebuffer, gpuFrameWidth, gpuFramebufferScanlineStrideBytes, gpuFrameHeight, statsFrameSkipText, strlen(fpsText)*6, 1, RGB565(31,0,0), 0);
#ifdef USE_SPI_THREAD
  DrawText(framebuffer, gpuFrameWidth, gpuFramebufferScanlineStrideBytes, gpuFrameHeight, spiUsagePercentageText, 45, 1, spiUsageColor, 0);
#endif
  DrawText(framebuffer, gpuFrameWidth, gpuFramebufferScanlineStrideBytes, gpuFrameHeight, spiBusDataRateText, 75, 1, 0xFFFF, 0);
  DrawText(framebuffer, gpuFrameWidth, gpuFramebufferScanlineStrideBytes, gpuFrameHeight, spiSpeedText, 140, 1, RGB565(31,14,20), 0);
  DrawText(framebuffer, gpuFrameWidth, gpuFramebufferScanlineStrideBytes, gpuFrameHeight, cpuTemperatureText, 210, 1, cpuTemperatureColor, 0);
  DrawText(framebuffer, gpuFrameWidth, gpuFramebufferScanlineStrideBytes, gpuFrameHeight, gpuPollingWastedText, 242, 1, gpuPollingWastedColor, 0);
}

void RefreshStatisticsOverlayText()
{
  uint64_t now = tick();
  uint64_t elapsed = now - statsLastPrint;
  if (elapsed < STATISTICS_REFRESH_INTERVAL) return;

#ifdef KERNEL_MODULE_CLIENT
  spiThreadUtilizationRate = 0; // TODO
  int spiRate = 0;
  strcpy(spiUsagePercentageText, "N/A");
#else
  uint64_t spiThreadIdleFor = __atomic_load_n(&spiThreadIdleUsecs, __ATOMIC_RELAXED);
  __sync_fetch_and_sub(&spiThreadIdleUsecs, spiThreadIdleFor);
  if (__atomic_load_n(&spiThreadSleeping, __ATOMIC_RELAXED)) spiThreadIdleFor += tick() - spiThreadSleepStartTime;
  spiThreadUtilizationRate = MIN(1.0, MAX(0.0, 1.0 - spiThreadIdleFor / (double)STATISTICS_REFRESH_INTERVAL));
  int spiRate = (int)MIN(100, (spiThreadUtilizationRate*100.0));
  sprintf(spiUsagePercentageText, "%d%%", spiRate);
#endif
  spiBusDataRate = (double)8.0 * statsBytesTransferred * 1000.0 / (elapsed / 1000.0);

  if (spiRate < 90) spiUsageColor = RGB565(0,63,0);
  else if (spiRate < 100) spiUsageColor = RGB565(31,63,0);
  else spiUsageColor = RGB565(31,0, 0);

  if (spiBusDataRate > 1000000) sprintf(spiBusDataRateText, "%.2fmbps", spiBusDataRate/1000000.0);
  else if (spiBusDataRate > 1000) sprintf(spiBusDataRateText, "%.2fkbps", spiBusDataRate/1000.0);
  else sprintf(spiBusDataRateText, "%.2fbps", spiBusDataRate);

  uint64_t wastedTime = __atomic_load_n(&timeWastedPollingGPU, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&timeWastedPollingGPU, wastedTime, __ATOMIC_RELAXED);
  //const double gpuPollingWastedScalingFactor = 0.369; // A crude heuristic to scale time spent in useless polling to what Linux 'top' tool shows as % usage percentages
  statsGpuPollingWasted = (int)(wastedTime /** gpuPollingWastedScalingFactor*/ * 100 / (now - statsLastPrint));

  statsBytesTransferred = 0;

  if (statsSpiBusSpeed > 0 && statsCpuFrequency > 0) sprintf(spiSpeedText, "%d/%dMHz", statsCpuFrequency, statsSpiBusSpeed);
  else spiSpeedText[0] = '\0';

  if (statsCpuTemperature > 0)
  {
    sprintf(cpuTemperatureText, "%.1fc", statsCpuTemperature);
    if (statsCpuTemperature >= 80) cpuTemperatureColor = RGB565(31, 0, 0);
    else if (statsCpuTemperature >= 65) cpuTemperatureColor = RGB565(31, 63, 0);
    else cpuTemperatureColor = RGB565(0, 63, 0);
  }

  if (statsGpuPollingWasted > 0)
  {
    gpuPollingWastedColor = (statsGpuPollingWasted > 5) ? RGB565(31, 0, 0) : RGB565(31, 63, 0);
    sprintf(gpuPollingWastedText, "+%d%%", statsGpuPollingWasted);
  }
  else gpuPollingWastedText[0] = '\0';

  statsLastPrint = now;

  if (frameTimeHistorySize >= 3)
  {
    int numInterlacedFramesInHistory = false;
    for(int i = 0; i < frameTimeHistorySize; ++i)
      if (frameTimeHistory[i].interlaced)
        ++numInterlacedFramesInHistory;

    int frames = frameTimeHistorySize;
    if (numInterlacedFramesInHistory)
      for(int i = 0; i < frameTimeHistorySize; ++i)
        if (!frameTimeHistory[i].interlaced) ++frames; // Progressive frames count twice
    int fps = (0.5 + (frames - 1) * 1000000.0 / (frameTimeHistory[frameTimeHistorySize-1].time - frameTimeHistory[0].time));
#ifdef NO_INTERLACING
    sprintf(fpsText, "%d", fps);
    fpsColor = 0xFFFF;
#else
    if (numInterlacedFramesInHistory > 0)
    {
      sprintf(fpsText, "%di/%d", fps, numInterlacedFramesInHistory);
      fpsColor = RGB565(31, 30, 11);
    }
    else
    {
      sprintf(fpsText, "%dp", fps);
      fpsColor = 0xFFFF;
    }
#endif
    if (frameSkipTimeHistorySize > 0) sprintf(statsFrameSkipText, "-%d", frameSkipTimeHistorySize);
    else statsFrameSkipText[0] = '\0';
  }
  else
  {
    strcpy(fpsText, "-");
    statsFrameSkipText[0] = '\0';
    fpsColor = 0xFFFF;
  }
}
#else
int InitStatistics() {}
void RefreshStatisticsOverlayText() {}
void DrawStatisticsOverlay(uint16_t *) {}
#endif // ~STATISTICS
