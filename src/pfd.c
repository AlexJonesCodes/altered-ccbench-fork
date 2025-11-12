/*   
 *   File: pfd.c
 *   Author: Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>
 *   Description: a fine-grained profiler based on rdtsc
 *   pfd.c is part of ccbench
 *
 * The MIT License (MIT)
 *
 * Copyright (C) 2013  Vasileios Trigonakis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

 // pfd essentially measures the time taken for a selected instruction to execute
 // it uses rdtsc to measure the time before and after the instruction
 // it does so in a way that minimizes the overhead of the measurement itself
 // by measuring the before and after ticks, then subtracting the overhead of the rdtsc calls themselves

#include "pfd.h"
#include <math.h>
#include "atomic_ops.h"

#define PFD_CONSERVATIVE_DEFAULT 32.0

THREAD_LOCAL volatile ticks** pfd_store;
THREAD_LOCAL volatile ticks* _pfd_s;
THREAD_LOCAL volatile ticks pfd_correction;

static ticks
measure_minimum_tick_delta(uint32_t attempts)
{
  ticks best = UINT64_MAX;

  for (uint32_t i = 0; i < attempts; i++)
    {
      ticks start = getticks();
      asm volatile ("" ::: "memory");
      ticks end = getticks();
      ticks delta = end - start;

      if (delta > 0 && delta < best)
        {
          best = delta;
        }
    }

  if (best == UINT64_MAX)
    {
      return 0;
    }

  return best;
}

static int
ticks_compare(const void* lhs, const void* rhs)
{
  const ticks a = *(const ticks*) lhs;
  const ticks b = *(const ticks*) rhs;

  if (a < b)
    {
      return -1;
    }
  if (a > b)
    {
      return 1;
    }
  return 0;
}

static double
median_non_zero_ticks(const volatile ticks* samples, uint32_t num_entries)
{
  if (num_entries == 0)
    {
      return NAN;
    }

  ticks* scratch = (ticks*) malloc(num_entries * sizeof(ticks));
  if (scratch == NULL)
    {
      return NAN;
    }

  size_t count = 0;
  for (uint32_t i = 0; i < num_entries; i++)
    {
      ticks value = samples[i];
      if (value == 0)
        {
          continue;
        }
      scratch[count++] = value;
    }

  if (count == 0)
    {
      free(scratch);
      return NAN;
    }

  qsort(scratch, count, sizeof(ticks), ticks_compare);

  double median;
  if ((count & 1) == 0)
    {
      median = ((double) scratch[count / 2 - 1] + (double) scratch[count / 2]) / 2.0;
    }
  else
    {
      median = (double) scratch[count / 2];
    }

  free(scratch);
  return median;
}

void 
pfd_store_init(uint32_t num_entries)
{
  if (pfd_store != NULL)
    {
      for (uint32_t i = 0; i < PFD_NUM_STORES; i++)
        {
          free((void*) pfd_store[i]);
          pfd_store[i] = NULL;
        }
      free((void*) pfd_store);
      pfd_store = NULL;
    }

  free((void*) _pfd_s);
  _pfd_s = NULL;

  _pfd_s = (volatile ticks*) malloc(PFD_NUM_STORES * sizeof(ticks));
  pfd_store = (volatile ticks**) malloc(PFD_NUM_STORES * sizeof(ticks*));
  assert(_pfd_s != NULL && pfd_store != NULL);

  volatile uint32_t i;
  for (i = 0; i < PFD_NUM_STORES; i++)
    {
      pfd_store[i] = (ticks*) malloc(num_entries * sizeof(ticks));
      assert(pfd_store[i] != NULL);
      PREFETCHW((void*) &pfd_store[i][0]);
    }

  int32_t tries = 10;
  uint32_t print_warning = 0;


#if defined(XEON) || defined(OPTERON2) || defined(XEON2) || defined(DEFAULT) || defined(i3_7020U)
  /* enforcing max freq if freq scaling is enabled */
  volatile uint64_t speed;
  for (speed = 0; speed < 20e7; speed++)
    {
      asm volatile ("");
    }
#endif	/* XEON */

  pfd_correction = 0;

#define PFD_CORRECTION_CONF 3
 retry:
  for (i = 0; i < num_entries; i++)
    {
      PFDI(0);
      asm volatile ("");
      PFDO(0, i);
    }

  abs_deviation_t ad;
  get_abs_deviation(pfd_store[0], num_entries, &ad);
  double std_pp = NAN;
  if (isfinite(ad.avg) && ad.avg != 0.0)
    {
      std_pp = 100 * (1 - (ad.avg - ad.std_dev) / ad.avg);
    }

  if (std_pp > PFD_CORRECTION_CONF)
    {
      if (print_warning++ == 1)	/* print warning if 2 failed attempts */
	{
          double printed_std_pp = isfinite(std_pp) ? std_pp : 0.0;
          printf("* warning: avg pfd correction is %.1f with std deviation: %.1f%%. Recalculating.\n",
                 ad.avg, printed_std_pp);
	}
      if (tries-- > 0)
	{
	  goto retry;
	}
      else
        {
          printf("* warning: setting pfd correction manually\n");
          double manual_avg = median_non_zero_ticks(pfd_store[0], num_entries);
          if (isfinite(manual_avg) && manual_avg > 0)
            {
              ad.avg = manual_avg;
              printf("* warning: using median pfd correction of %.1f cycles after repeated retries.\n",
                     ad.avg);
            }
          else
            {
#if defined(OPTERON)
              ad.avg = 64;
#elif defined(OPTERON2)
              ad.avg = 68;
#elif defined(XEON) || defined(XEON2)
              ad.avg = 20;
#elif defined(NIAGARA)
              ad.avg = 76;
#elif defined(RYZEN53600)
              ad.avg = 32;
#elif defined(i3_7020U)
              ad.avg = 25;
#else
              printf("* warning: unknown architecture; using conservative pfd correction default of %.0f cycles.\n",
                     PFD_CONSERVATIVE_DEFAULT);
              /* Ensure that we still end up with a positive correction value even
                 when running on an unknown architecture.  The exact value is not
                 critical (it simply compensates for measurement overhead), but it
                 must be greater than zero to keep the profiler functional.  Use a
                 conservative default that mirrors the values used on similar x86
                 systems. */
              ad.avg = PFD_CONSERVATIVE_DEFAULT;
#endif
            }
        }
    }

  double corrected_avg = ad.avg;
  ticks correction = 0;

  if (!isfinite(corrected_avg))
    {
      /* The computed average can become NaN or +/-Inf when the raw samples
         are all identical (e.g. constant zero) and the standard deviation
         logic divides by zero.  Fall back to a conservative default rather
         than propagating the NaN and tripping the assertion below. */
      corrected_avg = PFD_CONSERVATIVE_DEFAULT;
      printf("* warning: measured pfd correction is non-finite; using conservative default of %.0f.\n",
             corrected_avg);
    }
  else if (corrected_avg <= 0)
    {
      /* When the measured correction is zero or negative it means that the
         profiling overhead is too small to be observed accurately on this
         platform (e.g. due to coarse timers or aggressive virtualisation).
         Ensure we still subtract a sensible positive value so that later
         computations never underflow.  Prefer a directly measured TSC delta,
         falling back to a conservative constant if the timer never advanced. */
      ticks measured = measure_minimum_tick_delta(64);
      if (measured > 0)
        {
          corrected_avg = (double) measured;
          printf("* warning: measured pfd correction <= 0; using direct rdtsc delta of %llu.\n",
                 (long long unsigned int) measured);
        }
      else
        {
          corrected_avg = PFD_CONSERVATIVE_DEFAULT;
          printf("* warning: measured pfd correction <= 0; using conservative default of %.0f.\n",
                 corrected_avg);
        }
    }
  else if (corrected_avg < 1.0)
    {
      /* Extremely small (sub-cycle) averages will truncate to zero when cast
         to ticks.  These values appear on systems with very noisy timing
         sources.  Clamp to the minimum meaningful correction. */
      corrected_avg = 1.0;
      printf("* warning: measured pfd correction < 1; clamping to %.0f.\n",
             corrected_avg);
    }

  if (corrected_avg >= (double) UINT64_MAX)
    {
      /* Guard against unrealistic averages overflowing the ticks type.  This
         situation should not occur in practice, but clamping avoids undefined
         behaviour in the conversion. */
      correction = UINT64_MAX;
      corrected_avg = (double) correction;
      printf("* warning: measured pfd correction >= UINT64_MAX; clamping to %llu\n",
             (long long unsigned int) correction);
    }
  else
    {
      correction = (ticks) (corrected_avg + 0.5);
      if (correction == 0)
        {
          /* Rounding still produced zero (e.g. due to subnormal doubles).  Use
             the minimum positive correction so the profiler remains usable. */
          correction = 1;
          corrected_avg = (double) correction;
          printf("* warning: rounded pfd correction was 0; clamping to %llu\n",
                 (long long unsigned int) correction);
        }
    }

  if (ad.avg <= 0)
    {
      /* When the measured correction is zero or negative it means that the
         profiling overhead is too small to be observed accurately on this
         platform (e.g. due to coarse timers or aggressive virtualisation).
         Ensure we still subtract a sensible positive value so that later
         computations never underflow.  Use the same conservative fallback as
         the unknown-architecture branch above. */
      ad.avg = PFD_CONSERVATIVE_DEFAULT;
      printf("* warning: measured pfd correction <= 0; using conservative default of %.0f.\n",
             ad.avg);
    }

  ad.avg = corrected_avg;
  pfd_correction = correction;
  if (pfd_correction <= 0)
    {
      /* The adjustment can still end up zero when the rdtsc delta never
         increased above the noise floor.  Try another direct measurement
         before falling back to the conservative constant. */
      ticks measured = measure_minimum_tick_delta(64);
      if (measured > 0)
        {
          pfd_correction = measured;
          corrected_avg = (double) pfd_correction;
          ad.avg = corrected_avg;
          if (!isfinite(std_pp))
            {
              std_pp = 0.0;
            }
          printf("* warning: enforcing positive pfd correction via direct rdtsc delta of %llu cycles.\n",
                 (long long unsigned int) pfd_correction);
        }
      else
        {
          pfd_correction = (ticks) (PFD_CONSERVATIVE_DEFAULT + 0.5);
          if (pfd_correction == 0)
            {
              pfd_correction = 1;
            }
          corrected_avg = (double) pfd_correction;
          ad.avg = corrected_avg;
          if (!isfinite(std_pp))
            {
              std_pp = 0.0;
            }
          printf("* warning: falling back to conservative pfd correction of %llu cycles.\n",
                 (long long unsigned int) pfd_correction);
        }
    }

  assert(pfd_correction > 0);

  double printed_std_pp = isfinite(std_pp) ? std_pp : 0.0;
  printf("* set pfd correction: %llu (std deviation: %.1f%%)\n",
         (long long unsigned int) pfd_correction, printed_std_pp);
}

static inline 
double absd(double x)
{
  if (x >= 0)
    {
      return x;
    }
  else 
    {
      return -x;
    }
}


#define llu long long unsigned int
void 
print_abs_deviation(const abs_deviation_t* abs_dev)
{
  printf("\n ---- statistics:\n");
  PRINT("    avg : %-10.1f abs dev : %-10.1f std dev : %-10.1f num     : %llu",
        abs_dev->avg, abs_dev->abs_dev, abs_dev->std_dev, (llu) abs_dev->num_vals);
  PRINT("    min : %-10.1f (element: %6llu)    max     : %-10.1f (element: %6llu)", abs_dev->min_val, 
	(llu) abs_dev->min_val_idx, abs_dev->max_val, (llu) abs_dev->max_val_idx);
  double v10p = 100 * 
    (1 - (abs_dev->num_vals - abs_dev->num_dev_10p) / (double) abs_dev->num_vals);
  double std_10pp = 100 * (1 - (abs_dev->avg_10p - abs_dev->std_dev_10p) / abs_dev->avg_10p);
  PRINT("  0-10%% : %-10u ( %5.1f%%  |  avg:  %6.1f  |  abs dev: %6.1f  |  std dev: %6.1f = %5.1f%% )", 
	abs_dev->num_dev_10p, v10p, abs_dev->avg_10p, abs_dev->abs_dev_10p, abs_dev->std_dev_10p, std_10pp);
  double v25p = 100 
    * (1 - (abs_dev->num_vals - abs_dev->num_dev_25p) / (double) abs_dev->num_vals);
  double std_25pp = 100 * (1 - (abs_dev->avg_25p - abs_dev->std_dev_25p) / abs_dev->avg_25p);
  PRINT(" 10-25%% : %-10u ( %5.1f%%  |  avg:  %6.1f  |  abs dev: %6.1f  |  std dev: %6.1f = %5.1f%% )", 
	abs_dev->num_dev_25p, v25p, abs_dev->avg_25p, abs_dev->abs_dev_25p, abs_dev->std_dev_25p, std_25pp);
  double v50p = 100 * 
    (1 - (abs_dev->num_vals - abs_dev->num_dev_50p) / (double) abs_dev->num_vals);
  double std_50pp = 100 * (1 - (abs_dev->avg_50p - abs_dev->std_dev_50p) / abs_dev->avg_50p);
  PRINT(" 25-50%% : %-10u ( %5.1f%%  |  avg:  %6.1f  |  abs dev: %6.1f  |  std dev: %6.1f = %5.1f%% )",
        abs_dev->num_dev_50p, v50p, abs_dev->avg_50p, abs_dev->abs_dev_50p, abs_dev->std_dev_50p, std_50pp);
  double v75p = 100 * 
    (1 - (abs_dev->num_vals - abs_dev->num_dev_75p) / (double) abs_dev->num_vals);
  double std_75pp = 100 * (1 - (abs_dev->avg_75p - abs_dev->std_dev_75p) / abs_dev->avg_75p);
  PRINT(" 50-75%% : %-10u ( %5.1f%%  |  avg:  %6.1f  |  abs dev: %6.1f  |  std dev: %6.1f = %5.1f%% )", 
	abs_dev->num_dev_75p, v75p, abs_dev->avg_75p, abs_dev->abs_dev_75p, abs_dev->std_dev_75p, std_75pp);
  double vrest = 100 * 
    (1 - (abs_dev->num_vals - abs_dev->num_dev_rst) / (double) abs_dev->num_vals);
  double std_rspp = 100 * (1 - (abs_dev->avg_rst - abs_dev->std_dev_rst) / abs_dev->avg_rst);
  PRINT("75-100%% : %-10u ( %5.1f%%  |  avg:  %6.1f  |  abs dev: %6.1f  |  std dev: %6.1f = %5.1f%% )\n", 
	abs_dev->num_dev_rst, vrest, abs_dev->avg_rst, abs_dev->abs_dev_rst, abs_dev->std_dev_rst, std_rspp);
}

void
pfd_collect_abs_deviation(uint32_t store, uint32_t num_vals, uint32_t num_print,
                          abs_deviation_t* out)
{
  uint32_t p = num_print;
  if (p > num_vals)
    {
      p = num_vals;
    }

  for (uint32_t i = 0; i < p; i++)
    {
      printf("[%3d: %4ld] ", i, (long int) pfd_store[store][i]);
    }

  abs_deviation_t ad;
  get_abs_deviation(pfd_store[store], num_vals, &ad);
  print_abs_deviation(&ad);

  if (out != NULL)
    {
      *out = ad;
    }
}

#define PFD_VAL_UP_LIMIT 1500	/* do not consider values higher than this value */

void
get_abs_deviation(volatile ticks* vals, const size_t num_vals, abs_deviation_t* abs_dev)
{
  abs_dev->num_vals = num_vals;
  ticks sum_vals = 0;
  uint32_t i;
  for (i = 0; i < num_vals; i++)
    {
      if ((int64_t) vals[i] < 0 || vals[i] > PFD_VAL_UP_LIMIT)
	{
	  vals[i] = 0;
	}
      sum_vals += vals[i];
    }

  double avg = sum_vals / (double) num_vals;
  abs_dev->avg = avg;
  double max_val = 0;
  double min_val = DBL_MAX;
  uint64_t max_val_idx = 0, min_val_idx = 0;
  uint32_t num_dev_10p = 0; ticks sum_vals_10p = 0; double dev_10p = 0.1 * avg;
  uint32_t num_dev_25p = 0; ticks sum_vals_25p = 0; double dev_25p = 0.25 * avg;
  uint32_t num_dev_50p = 0; ticks sum_vals_50p = 0; double dev_50p = 0.5 * avg;
  uint32_t num_dev_75p = 0; ticks sum_vals_75p = 0; double dev_75p = 0.75 * avg;
  uint32_t num_dev_rst = 0; ticks sum_vals_rst = 0;

  double sum_adev = 0;		/* abs deviation */
  double sum_stdev = 0;		/* std deviation */
  for (i = 0; i < num_vals; i++)
    {
      double diff = vals[i] - avg;
      double ad = absd(diff);
      if (vals[i] > max_val)
	{
	  max_val = vals[i];
	  max_val_idx = i;
	}
      else if (vals[i] < min_val)
	{
	  min_val = vals[i];
	  min_val_idx = i;
	}

      if (ad <= dev_10p)
	{
	  num_dev_10p++;
	  sum_vals_10p += vals[i];
	}
      else if (ad <= dev_25p)
	{
	  num_dev_25p++;
	  sum_vals_25p += vals[i];
	}
      else if (ad <= dev_50p)
	{
	  num_dev_50p++;
	  sum_vals_50p += vals[i];
	}
      else if (ad <= dev_75p)
	{
	  num_dev_75p++;
	  sum_vals_75p += vals[i];
	}
      else
	{
	  num_dev_rst++;
	  sum_vals_rst += vals[i];
	}

      sum_adev += ad;
      sum_stdev += ad*ad;
    }
  abs_dev->min_val = min_val;
  abs_dev->min_val_idx = min_val_idx;
  abs_dev->max_val = max_val;
  abs_dev->max_val_idx = max_val_idx;
  abs_dev->num_dev_10p = num_dev_10p;
  abs_dev->num_dev_25p = num_dev_25p;
  abs_dev->num_dev_50p = num_dev_50p;
  abs_dev->num_dev_75p = num_dev_75p;
  abs_dev->num_dev_rst = num_dev_rst;

  abs_dev->avg_10p = sum_vals_10p / (double) num_dev_10p;
  abs_dev->avg_25p = sum_vals_25p / (double) num_dev_25p;
  abs_dev->avg_50p = sum_vals_50p / (double) num_dev_50p;
  abs_dev->avg_75p = sum_vals_75p / (double) num_dev_75p;
  abs_dev->avg_rst = sum_vals_rst / (double) num_dev_rst;

  double sum_adev_10p = 0, sum_adev_25p = 0, sum_adev_50p = 0, sum_adev_75p = 0, sum_adev_rst = 0;
  double sum_stdev_10p = 0, sum_stdev_25p = 0, sum_stdev_50p = 0, sum_stdev_75p = 0, sum_stdev_rst = 0;

  /* pass again to calculate the deviations for the 10/25..p */
  for (i = 0; i < num_vals; i++)
    {
      double diff = vals[i] - avg;
      double ad = absd(diff);
      if (ad <= dev_10p)
	{
	  double diff = vals[i] - abs_dev->avg_10p;
	  double ad = absd(diff);
	  sum_adev_10p += ad;
	  sum_stdev_10p += (ad*ad);
	}
      else if (ad <= dev_25p)
	{
	  double diff = vals[i] - abs_dev->avg_25p;
	  double ad = absd(diff);
	  sum_adev_25p += ad;
	  sum_stdev_25p += (ad*ad);
	}
      else if (ad <= dev_50p)
	{
	  double diff = vals[i] - abs_dev->avg_50p;
	  double ad = absd(diff);
	  sum_adev_50p += ad;
	  sum_stdev_50p += (ad*ad);
	}
      else if (ad <= dev_75p)
	{
	  double diff = vals[i] - abs_dev->avg_75p;
	  double ad = absd(diff);
	  sum_adev_75p += ad;
	  sum_stdev_75p += (ad*ad);
	}
      else
	{
	  double diff = vals[i] - abs_dev->avg_rst;
	  double ad = absd(diff);
	  sum_adev_rst += ad;
	  sum_stdev_rst += (ad*ad);
	}
    }

  abs_dev->abs_dev_10p = sum_adev_10p / num_dev_10p; 
  abs_dev->abs_dev_25p = sum_adev_25p / num_dev_25p; 
  abs_dev->abs_dev_50p = sum_adev_50p / num_dev_50p; 
  abs_dev->abs_dev_75p = sum_adev_75p / num_dev_75p; 
  abs_dev->abs_dev_rst = sum_adev_rst / num_dev_rst; 

  abs_dev->std_dev_10p = sqrt(sum_stdev_10p / num_dev_10p); 
  abs_dev->std_dev_25p = sqrt(sum_stdev_25p / num_dev_25p); 
  abs_dev->std_dev_50p = sqrt(sum_stdev_50p / num_dev_50p); 
  abs_dev->std_dev_75p = sqrt(sum_stdev_75p / num_dev_75p); 
  abs_dev->std_dev_rst = sqrt(sum_stdev_rst / num_dev_rst); 

  double adev = sum_adev / num_vals;
  abs_dev->abs_dev = adev;
  double stdev = sqrt(sum_stdev / num_vals);
  abs_dev->std_dev = stdev;
}
