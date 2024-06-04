/*
 * Copyright (c) 2024, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Author: Noor ul Mubeen <noor.u.mubeen@intel.com>
 */

#define _GNU_SOURCE
#include <sched.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include "../wlt_proxy/common.h"
#include "../wlt_proxy/cpu_group.h"

#include "visualize_common.h"

FILE *gp;
extern size_t size_cpumask;


/* last few utilization values to show agaist flow of time*/
static float prev1, prev2, prev3, prev4;

/* radii to plot perf-per-watt semi-circular dial */
static float r1 = 0.9;
static float r2 = 0.8;
static float r3 = 0.7;
static float pi = 3.141;

/* some X-Y corner cordinates for sub-plots */
#define UTIL_X  -1.6
#define UTIL_Y  3.6
#define UTIL_WD  0.9
#define UTIL_HT  1.2

#define STATES_X  -1.6
#define STATES_Y  1.25
#define STATES_WD  3.4
#define STATES_HT  1.4

#define PPW_X  -1.6
#define PPW_Y  -0.6
#define PPW_WD  3.4
#define PPW_HT  1.7

#define TOPOLOGY_X  -0.5
#define TOPOLOGY_Y  2.8
#define TOPOLOGY_WD  2.3
#define TOPOLOGY_HT  2.0

#define CPU_W_PAD_HT  0.3
#define CPU_W_PAD_WD  0.5
#define CPU_PAD_HT  0.1
#define CPU_PAD_WD  0.2
#define CPU_BASE_X  (TOPOLOGY_X + 0.15)
#define CPU_BASE_Y  (TOPOLOGY_Y + 0.1)
#define CPU_TOP_BASE_Y  (TOPOLOGY_Y + 1.8)
#define ALL_CPU_HT  1.8
#define ALL_CPU_WD  2.0
#define NUM_CPU_COLS 4

int state_text_template(void)
{
	for (int m = 0; m < get_mode_max(); m++) {
		fprintf(gp,
			"set label %d \"[%d] %s cpu# %d EPP:%x EPB:%x poll:%4d elasticity:%d\" at %.2f,%.2f tc rgb \"%s\";",
			100 + m, m, get_mode_name(m), get_mode_cpu_count(m),
			get_state_epp(m), get_state_epb(m), get_poll_ms(m),
			get_state_poll_order(m), STATES_X + 0.15,
			STATES_Y + STATES_HT - 0.2 - (float)(1.4 * m) / 10,
			is_state_valid(m) ? "black" : "grey");
	}
	return 1;
}

int update_once()
{
	/* parametric functions add utilization overhead. avoid it if updates are frequent */
	fprintf(gp, "set parametric;");
	fprintf(gp, "set trange [0:1];");
	fprintf(gp, "set samples 100;");

	fprintf(gp, "r1 = %.2f;", r1);
	fprintf(gp, "r2 = %.2f;", r2);
	fprintf(gp, "r3 = %.2f;", r3);
	fprintf(gp, "p1a(t)= t*pi - pi/2;");
	fprintf(gp,
		"plot r1*sin(p1a(t)),r1*cos(p1a(t)) w points pt 0 lc rgb \"blue\";");
	fprintf(gp,
		"replot r2*sin(p1a(t)),r2*cos(p1a(t)) w points pt 0 lc rgb \"green\";");
	fprintf(gp,
		"replot r3*sin(p1a(t)),r3*cos(p1a(t)) w points pt 0 lc rgb \"red\";");
	fflush(gp);
	return 1;
}

struct _freq_map fmap;
int update_cpu()
{
	int cpu_in_bucket;
	float xx, yy = 0.0;
	int freq_buckets = get_freq_map_count();
	float cpu_top_base_y = CPU_TOP_BASE_Y;
	for (int j = 0; j < freq_buckets; j++) {
		get_freq_map(j, &fmap);
		cpu_in_bucket = fmap.end_cpu - fmap.start_cpu + 1;
		//printf("bucket [%d] start %d end %d\n", j, fmap.start_cpu,
		//      fmap.end_cpu);
		cpu_top_base_y = (yy == 0) ? cpu_top_base_y - 0.15 : yy - 0.15;
		for (int i = 0; i < cpu_in_bucket; i++) {
			int cpu_num = fmap.start_cpu + i;
			xx = CPU_BASE_X + (i % NUM_CPU_COLS) * CPU_W_PAD_WD;
			yy = cpu_top_base_y - (i / NUM_CPU_COLS) * CPU_W_PAD_HT;
			//printf
			//    ("set object %d rect from %.2f,%.2f to %.2f,%.2f \n",
			//    200 + cpu_num, xx, yy, xx + CPU_PAD_WD,
			//     yy + CPU_PAD_HT);
			fprintf(gp,
				"set object %d rect from %.2f,%.2f to %.2f,%.2f front fc rgb \"red\" lw 1 lc rgb '#ff0000';",
				200 + cpu_num, xx, yy, xx + CPU_PAD_WD,
				yy + CPU_PAD_HT);
		}
	}
	return 1;
}

static int paint_template(void)
{
	printf("Initializing gnuplt cmds\n");
	fprintf(gp, "set term wxt size 800,800 font \"Monospace,8\" ;");
	fprintf(gp, "set xrange [-2:2];");
	fprintf(gp, "set yrange [-1:5];");
	fprintf(gp, "set size ratio -1;");
	fprintf(gp, "unset key;");
	fprintf(gp, "unset tics;");
	fprintf(gp, "set style circle radius 0.5;");

	fprintf(gp, "set object 11  rect from %.2f,%.2f to %.2f,%.2f behind;",
		UTIL_X, UTIL_Y, UTIL_X + UTIL_WD, UTIL_Y + UTIL_HT);
	fprintf(gp, "set label 12 \"Utilization\"  at %.2f,%.2f front;",
		UTIL_X + 0.2, UTIL_Y + 0.15);

	fprintf(gp, "set object 21 rect from %.2f,%.2f to %.2f,%.2f behind;",
		TOPOLOGY_X, TOPOLOGY_Y, TOPOLOGY_X + TOPOLOGY_WD,
		TOPOLOGY_Y + TOPOLOGY_HT);
	fprintf(gp,
		"set label 22 \"CPU topology (Active Cgroup)\" at %.2f,%.2f;",
		TOPOLOGY_X + 0.5, TOPOLOGY_Y + 0.1);

	fprintf(gp, "set object 31 rect from %.2f,%.2f to %.2f,%.2f behind;",
		STATES_X, STATES_Y, STATES_X + STATES_WD, STATES_Y + STATES_HT);

	fprintf(gp, "set object 41 rect from %.2f,%.2f to %.2f,%.2f behind;",
		PPW_X, PPW_Y, PPW_X + PPW_WD, PPW_Y + PPW_HT);
	fprintf(gp, "set label 45 \"Perf .....:  : :\" at %.2f,%.2f front;",
		PPW_X + 0.2, PPW_Y + 0.52);
	fprintf(gp, "set label 46 \"Perf÷Watt ...: :\" at %.2f,%.2f front;",
		PPW_X + 0.2, PPW_Y + 0.41);
	fprintf(gp, "set label 47 \"Watt ..........:\" at %.2f,%.2f front;",
		PPW_X + 0.2, PPW_Y + 0.30);

	update_once();
	state_text_template();
	update_cpu();
	fflush(gp);

	return 1;
}

/* recent few points on perf per watt dashboard dial */
#define tail_size 4
static float r1x[tail_size], r1y[tail_size];
static float r2x[tail_size], r2y[tail_size];
static float r3x[tail_size], r3y[tail_size];

static cpu_set_t *prev_cpuset, *now_cpuset;
static int prev_state;

/*
 * update function is optimized to keep minimal cmds pushed to gnuplot
 * e.g only difference in cpu box since last updated to repaint are updated.
 * also some cmds cause more cpu usage by gnuplot. they have to be avoided.
 */
int update_plot(float R1, float R2, float R3, float max1, int m)
{
	if (R1 > 1)
		R1 = 1;
	if (R2 > 1)
		R2 = 1;
	if (R3 > 1)
		R3 = 1;
	if (R1 <= 0)
		R1 = 0.5;
	if (R2 <= 0)
		R2 = 0.5;
	if (R3 <= 0)
		R3 = 0.5;
	fprintf(gp, "R1 = %.2f;", R1);
	fprintf(gp, "R2 = %.2f;", R2);
	fprintf(gp, "R3 = %.2f;", R3);

	r1x[0] = r1 * sin(R1 * pi - pi / 2);
	r1y[0] = r1 * cos(R1 * pi - pi / 2);

	r2x[0] = r2 * sin(R2 * pi - pi / 2);
	r2y[0] = r2 * cos(R2 * pi - pi / 2);

	r3x[0] = r3 * sin(R3 * pi - pi / 2);
	r3y[0] = r3 * cos(R3 * pi - pi / 2);

	for (int tail = tail_size - 1; tail >= 0; tail--) {
		fprintf(gp,
			"set object %d circle at first %.2f,%.2f size 0.03 fillcolor rgb \"blue\" fs transparent solid %.2f noborder;",
			60 + tail, r1x[tail], r1y[tail],
			(float)(1) / (5 * tail * tail + 1));
		fprintf(gp,
			"set object %d circle at first %.2f,%.2f size 0.03 fillcolor rgb \"green\" fs transparent solid %.2f noborder;",
			70 + tail, r2x[tail], r2y[tail],
			(float)(1) / (5 * tail * tail + 1));
		fprintf(gp,
			"set object %d circle at first %.2f,%.2f size 0.03 fillcolor rgb \"red\" fs transparent solid %.2f noborder;",
			80 + tail, r3x[tail], r3y[tail],
			(float)(1) / (5 * tail * tail + 1));
		if (tail > 0) {
			r1x[tail] = r1x[tail - 1];
			r1y[tail] = r1y[tail - 1];

			r2x[tail] = r2x[tail - 1];
			r2y[tail] = r2y[tail - 1];

			r3x[tail] = r3x[tail - 1];
			r3y[tail] = r3y[tail - 1];
		}
	}

	fprintf(gp, "unset arrow;");
	float UTIL_Y_ARROW = UTIL_Y + 0.3;
	fprintf(gp, "set arrow from %.2f,%.2f to %.2f,%.2f lw 2.0 lt 4;", -1.0,
		UTIL_Y_ARROW, -1.0, UTIL_Y_ARROW + max1 / 2);
	fprintf(gp, "set arrow from %.2f,%.2f to %.2f,%.2f lw 1.5 lt 4;", -1.1,
		UTIL_Y_ARROW, -1.1, UTIL_Y_ARROW + prev1 / 2);
	fprintf(gp, "set arrow from %.2f,%.2f to %.2f,%.2f lw 1.0 lt 4;", -1.2,
		UTIL_Y_ARROW, -1.2, UTIL_Y_ARROW + prev2 / 2);
	fprintf(gp, "set arrow from %.2f,%.2f to %.2f,%.2f lw 0.5 lt 4;", -1.3,
		UTIL_Y_ARROW, -1.3, UTIL_Y_ARROW + prev3 / 2);
	fprintf(gp, "set arrow from %.2f,%.2f to %.2f,%.2f lw 0.25 lt 4;", -1.4,
		UTIL_Y_ARROW, -1.4, UTIL_Y_ARROW + prev4 / 2);
	fprintf(gp, "replot\n");
	fflush(gp);

	fprintf(gp, "max1 = %.2f;", max1);
	fprintf(gp, "prev1 = %.2f;", prev1);
	fprintf(gp, "prev2 = %.2f;", prev2);
	fprintf(gp, "prev3 = %.2f;", prev3);
	fprintf(gp, "prev4 = %.2f;", prev4);

	fprintf(gp, "unset object 100;");
	fprintf(gp,
		"set object 100 rect from %.2f,%.2f to %.2f,%.2f lw 3 fs empty border lc rgb '#ff0000';",
		STATES_X + 0.15,
		STATES_Y + STATES_HT - 0.25 - (float)(1.4 * m) / 10, 1.5,
		STATES_Y + STATES_HT - 0.15 - (float)(1.4 * m) / 10);
	if (prev_state != m) {
		now_cpuset = get_cpu_mask(m);
		cpu_set_t *tmp_cpuset, *setbits_cpuset, *clearbits_cpuset;
		alloc_cpu_set(&tmp_cpuset);
		alloc_cpu_set(&setbits_cpuset);
		alloc_cpu_set(&clearbits_cpuset);
		if (prev_cpuset) {
			CPU_ZERO_S(size_cpumask, tmp_cpuset);
			CPU_XOR_S(size_cpumask, tmp_cpuset, prev_cpuset,
				  now_cpuset);
			CPU_AND_S(size_cpumask, setbits_cpuset, tmp_cpuset,
				  now_cpuset);

			CPU_ZERO_S(size_cpumask, tmp_cpuset);
			CPU_OR_S(size_cpumask, tmp_cpuset, prev_cpuset,
				 now_cpuset);
			CPU_XOR_S(size_cpumask, clearbits_cpuset, tmp_cpuset,
				  now_cpuset);
		}

		int cpu_total = get_max_cpus();
		for (int i = 0; i < cpu_total; i++) {
			if (CPU_ISSET_S(i, size_cpumask, setbits_cpuset)) {
				fprintf(gp,
					"set object %d front fc rgb \"red\" lw 1 lc rgb '#ff0000';",
					200 + i);
			}

			if (CPU_ISSET_S(i, size_cpumask, clearbits_cpuset)) {
				fprintf(gp,
					"set object %d front fc rgb \"grey\" lw 1 lc rgb '#ff0000';",
					200 + i);
			}
		}
	}
	prev_cpuset = now_cpuset;
	prev_state = m;

	fflush(gp);
	prev4 = prev3;
	prev3 = prev2;
	prev2 = prev1;
	prev1 = max1;
	return 1;
}

int init_gnuplot(void)
{
	if (NULL == (gp = popen("/usr/bin/gnuplot", "w"))) {
		perror("gnuplot");
		pclose(gp);
		return 0;
	}
	printf("Connected to gnuplot.\n");
	paint_template();
	return 1;
}

int exit_gnuplot(void)
{
	if (gp)
		pclose(gp);
	return 1;
}
