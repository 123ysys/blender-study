/* SPDX-FileCopyrightText: 2016 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include <algorithm>

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_string.h"

#include "BKE_global.h"

#include "BLF_api.h"

#include "MEM_guardedalloc.h"

#include "draw_manager.h"

#include "GPU_debug.h"
#include "GPU_texture.h"

#include "UI_resources.hh"

#include "draw_manager_profiling.hh"

#define MAX_TIMER_NAME 32
#define MAX_NESTED_TIMER 8
#define MIM_RANGE_LEN 8
#define GPU_TIMER_FALLOFF 0.1

struct DRWTimer {
  uint32_t query[2];
  uint64_t time_average;
  char name[MAX_TIMER_NAME];
  int lvl;       /* Hierarchy level for nested timer. */
  bool is_query; /* Does this timer actually perform queries or is it just a group. */
};

static struct DRWTimerPool {
  DRWTimer *timers;
  int chunk_count;     /* Number of chunk allocated. */
  int timer_count;     /* chunk_count * CHUNK_SIZE */
  int timer_increment; /* Keep track of where we are in the stack. */
  int end_increment;   /* Keep track of bad usage. */
  bool is_recording;   /* Are we in the render loop? */
  bool is_querying;    /* Keep track of bad usage. */
} DTP = {nullptr};

void DRW_stats_free()
{
  if (DTP.timers != nullptr) {
    // for (int i = 0; i < DTP.timer_count; i++) {
    // DRWTimer *timer = &DTP.timers[i];
    // glDeleteQueries(2, timer->query);
    // }
    MEM_freeN(DTP.timers);
    DTP.timers = nullptr;
  }
}

void DRW_stats_begin()
{
  if (G.debug_value > 20 && G.debug_value < 30) {
    DTP.is_recording = true;
  }

  if (DTP.is_recording && DTP.timers == nullptr) {
    DTP.chunk_count = 1;
    DTP.timer_count = DTP.chunk_count * MIM_RANGE_LEN;
    DTP.timers = static_cast<DRWTimer *>(
        MEM_callocN(sizeof(DRWTimer) * DTP.timer_count, "DRWTimer stack"));
  }
  else if (!DTP.is_recording && DTP.timers != nullptr) {
    DRW_stats_free();
  }

  DTP.is_querying = false;
  DTP.timer_increment = 0;
  DTP.end_increment = 0;
}

static DRWTimer *drw_stats_timer_get()
{
  if (UNLIKELY(DTP.timer_increment >= DTP.timer_count)) {
    /* Resize the stack. */
    DTP.chunk_count++;
    DTP.timer_count = DTP.chunk_count * MIM_RANGE_LEN;
    DTP.timers = static_cast<DRWTimer *>(
        MEM_recallocN(DTP.timers, sizeof(DRWTimer) * DTP.timer_count));
  }

  return &DTP.timers[DTP.timer_increment++];
}

static void drw_stats_timer_start_ex(const char *name, const bool is_query)
{
  if (DTP.is_recording) {
    DRWTimer *timer = drw_stats_timer_get();
    STRNCPY(timer->name, name);
    timer->lvl = DTP.timer_increment - DTP.end_increment - 1;
    timer->is_query = is_query;

    /* Queries cannot be nested or interleaved. */
    BLI_assert(!DTP.is_querying);
    if (timer->is_query) {
      if (timer->query[0] == 0) {
        // glGenQueries(1, timer->query);
      }

      // glFinish();
      /* Issue query for the next frame */
      // glBeginQuery(GL_TIME_ELAPSED, timer->query[0]);
      DTP.is_querying = true;
    }
  }
}

void DRW_stats_group_start(const char *name)
{
  drw_stats_timer_start_ex(name, false);

  GPU_debug_group_begin(name);
}

void DRW_stats_group_end()
{
  GPU_debug_group_end();
  if (DTP.is_recording) {
    BLI_assert(!DTP.is_querying);
    DTP.end_increment++;
  }
}

void DRW_stats_query_start(const char *name)
{
  GPU_debug_group_begin(name);
  drw_stats_timer_start_ex(name, true);
}

void DRW_stats_query_end()
{
  GPU_debug_group_end();
  if (DTP.is_recording) {
    DTP.end_increment++;
    BLI_assert(DTP.is_querying);
    // glEndQuery(GL_TIME_ELAPSED);
    DTP.is_querying = false;
  }
}

void DRW_stats_reset()
{
  BLI_assert((DTP.timer_increment - DTP.end_increment) <= 0 &&
             "You forgot a DRW_stats_group/query_end somewhere!");
  BLI_assert((DTP.timer_increment - DTP.end_increment) >= 0 &&
             "You forgot a DRW_stats_group/query_start somewhere!");

  if (DTP.is_recording) {
    uint64_t lvl_time[MAX_NESTED_TIMER] = {0};

    /* Swap queries for the next frame and sum up each lvl time. */
    for (int i = DTP.timer_increment - 1; i >= 0; i--) {
      DRWTimer *timer = &DTP.timers[i];
      SWAP(uint32_t, timer->query[0], timer->query[1]);

      BLI_assert(timer->lvl < MAX_NESTED_TIMER);

      if (timer->is_query) {
        uint64_t time = 0;
        if (timer->query[0] != 0) {
          // glGetQueryObjectui64v(timer->query[0], GL_QUERY_RESULT, &time);
        }
        else {
          time = 1000000000; /* 1ms default */
        }

        timer->time_average = timer->time_average * (1.0 - GPU_TIMER_FALLOFF) +
                              time * GPU_TIMER_FALLOFF;
        timer->time_average = std::min(timer->time_average, uint64_t(1000000000));
      }
      else {
        timer->time_average = lvl_time[timer->lvl + 1];
        lvl_time[timer->lvl + 1] = 0;
      }

      lvl_time[timer->lvl] += timer->time_average;
    }

    DTP.is_recording = false;
  }
}

static void draw_stat_5row(const rcti *rect, int u, int v, const char *txt, const int size)
{
  BLF_draw_default_shadowed(rect->xmin + (1 + u * 5) * U.widget_unit,
                            rect->ymax - (3 + v) * U.widget_unit,
                            0.0f,
                            txt,
                            size);
}

static void draw_stat(const rcti *rect, int u, int v, const char *txt, const int size)
{
  BLF_draw_default_shadowed(
      rect->xmin + (1 + u) * U.widget_unit, rect->ymax - (3 + v) * U.widget_unit, 0.0f, txt, size);
}

void DRW_stats_draw(const rcti *rect)
{
  char stat_string[64];
  int lvl_index[MAX_NESTED_TIMER];
  int v = 0, u = 0;

  double init_tot_time = 0.0, background_tot_time = 0.0, render_tot_time = 0.0, tot_time = 0.0;

  int fontid = BLF_default();
  UI_FontThemeColor(fontid, TH_TEXT_HI);
  BLF_batch_draw_begin();

  /* ------------------------------------------ */
  /* ---------------- CPU stats --------------- */
  /* ------------------------------------------ */
  /* Label row */
  char col_label[32];
  STRNCPY(col_label, "Engine");
  draw_stat_5row(rect, u++, v, col_label, sizeof(col_label));
  STRNCPY(col_label, "Init");
  draw_stat_5row(rect, u++, v, col_label, sizeof(col_label));
  STRNCPY(col_label, "Background");
  draw_stat_5row(rect, u++, v, col_label, sizeof(col_label));
  STRNCPY(col_label, "Render");
  draw_stat_5row(rect, u++, v, col_label, sizeof(col_label));
  STRNCPY(col_label, "Total (w/o cache)");
  draw_stat_5row(rect, u++, v, col_label, sizeof(col_label));
  v++;

  /* Engines rows */
  char time_to_txt[16];
  DRW_ENABLED_ENGINE_ITER (DST.view_data_active, engine, data) {
    u = 0;

    draw_stat_5row(rect, u++, v, engine->idname, sizeof(engine->idname));

    init_tot_time += data->init_time;
    SNPRINTF(time_to_txt, "%.2fms", data->init_time);
    draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));

    background_tot_time += data->background_time;
    SNPRINTF(time_to_txt, "%.2fms", data->background_time);
    draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));

    render_tot_time += data->render_time;
    SNPRINTF(time_to_txt, "%.2fms", data->render_time);
    draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));

    tot_time += data->init_time + data->background_time + data->render_time;
    SNPRINTF(time_to_txt, "%.2fms", data->init_time + data->background_time + data->render_time);
    draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));
    v++;
  }

  /* Totals row */
  u = 0;
  STRNCPY(col_label, "Sub Total");
  draw_stat_5row(rect, u++, v, col_label, sizeof(col_label));
  SNPRINTF(time_to_txt, "%.2fms", init_tot_time);
  draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));
  SNPRINTF(time_to_txt, "%.2fms", background_tot_time);
  draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));
  SNPRINTF(time_to_txt, "%.2fms", render_tot_time);
  draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));
  SNPRINTF(time_to_txt, "%.2fms", tot_time);
  draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));
  v += 2;

  u = 0;
  double *cache_time = DRW_view_data_cache_time_get(DST.view_data_active);
  STRNCPY(col_label, "Cache Time");
  draw_stat_5row(rect, u++, v, col_label, sizeof(col_label));
  SNPRINTF(time_to_txt, "%.2fms", *cache_time);
  draw_stat_5row(rect, u++, v, time_to_txt, sizeof(time_to_txt));
  v += 2;

  /* ------------------------------------------ */
  /* ---------------- GPU stats --------------- */
  /* ------------------------------------------ */

  /* Memory Stats */
  uint tex_mem = GPU_texture_memory_usage_get();
  uint vbo_mem = GPU_vertbuf_get_memory_usage();

  STRNCPY(stat_string, "GPU Memory");
  draw_stat(rect, 0, v, stat_string, sizeof(stat_string));
  SNPRINTF(stat_string, "%.2fMB", double(tex_mem + vbo_mem) / 1000000.0);
  draw_stat_5row(rect, 1, v++, stat_string, sizeof(stat_string));
  STRNCPY(stat_string, "Textures");
  draw_stat(rect, 1, v, stat_string, sizeof(stat_string));
  SNPRINTF(stat_string, "%.2fMB", double(tex_mem) / 1000000.0);
  draw_stat_5row(rect, 1, v++, stat_string, sizeof(stat_string));
  STRNCPY(stat_string, "Meshes");
  draw_stat(rect, 1, v, stat_string, sizeof(stat_string));
  SNPRINTF(stat_string, "%.2fMB", double(vbo_mem) / 1000000.0);
  draw_stat_5row(rect, 1, v++, stat_string, sizeof(stat_string));
  v += 1;

  /* GPU Timings */
  STRNCPY(stat_string, "GPU Render Timings");
  draw_stat(rect, 0, v++, stat_string, sizeof(stat_string));

  for (int i = 0; i < DTP.timer_increment; i++) {
    double time_ms, time_percent;
    DRWTimer *timer = &DTP.timers[i];
    DRWTimer *timer_parent = (timer->lvl > 0) ? &DTP.timers[lvl_index[timer->lvl - 1]] : nullptr;

    /* Only display a number of lvl at a time */
    if ((G.debug_value - 21) < timer->lvl) {
      continue;
    }

    BLI_assert(timer->lvl < MAX_NESTED_TIMER);
    lvl_index[timer->lvl] = i;

    time_ms = timer->time_average / 1000000.0;

    if (timer_parent != nullptr) {
      time_percent = (double(timer->time_average) / double(timer_parent->time_average)) * 100.0;
    }
    else {
      time_percent = 100.0;
    }

    /* avoid very long number */
    time_ms = std::min(time_ms, 999.0);
    time_percent = std::min(time_percent, 100.0);

    SNPRINTF(stat_string, "%s", timer->name);
    draw_stat(rect, 0 + timer->lvl, v, stat_string, sizeof(stat_string));
    SNPRINTF(stat_string, "%.2fms", time_ms);
    draw_stat(rect, 12 + timer->lvl, v, stat_string, sizeof(stat_string));
    SNPRINTF(stat_string, "%.0f", time_percent);
    draw_stat(rect, 16 + timer->lvl, v, stat_string, sizeof(stat_string));
    v++;
  }

  BLF_batch_draw_end();
}
