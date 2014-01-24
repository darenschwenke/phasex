/*****************************************************************************
 *
 * wave.c
 *
 * PHASEX:  [P]hase [H]armonic [A]dvanced [S]ynthesis [EX]periment
 *
 * Copyright (C) 1999-2012 William Weston <whw@linuxmail.org>
 *
 * PHASEX is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PHASEX is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PHASEX.  If not, see <http://www.gnu.org/licenses/>.
 *
 *****************************************************************************/
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <samplerate.h>
#include "phasex.h"
#include "config.h"
#include "wave.h"
#include "filter.h"
#include "engine.h"
#include "patch.h"
#include "param_strings.h"
#include "settings.h"
#include "debug.h"


/* base tuning frequency -- can be overridden on command line */
double      a4freq = A4FREQ;

/* this is _the_ wave table */
sample_t    wave_table[NUM_WAVEFORMS][WAVEFORM_SIZE + 4];

/* frequency table for midi notes */
sample_t    freq_table[128][648];

/* lookup table for exponential frequency scaling */
sample_t    freq_shift_table[FREQ_SHIFT_TABLE_SIZE];

/* env table for envelope durations in samples */
int         env_table[128];

/* env curve for envelope values to actual amplitudes */
sample_t    env_curve[ENV_CURVE_SIZE + 1];

/* env interval durations, calculated per-interval */
int         env_interval_dur[6][128];

/* table for mixing gain, w/ unity at cc=127 */
sample_t    mix_table[128];

/* table for stereo panner, centered at 64 */
sample_t    pan_table[128];

/* table for general purpose gain, w/ unity at cc=120 */
sample_t    gain_table[128];

/* gain table for velocity[sensitivity][velicity], w/ unity at velocity=100 */
sample_t    velocity_gain_table[128][128];

/* keyfollow table for ctlr/key combo --> amplitude coefficient */
sample_t    keyfollow_table[128][128];


/*****************************************************************************
 * build_freq_table()
 *
 * Generates the (midi_note - 128) to frequency lookup table
 *****************************************************************************/
void
build_freq_table(void)
{
	double  basefreq;
	double  freq;
	double  halfstep;
	double  tunestep;
	int     octave;
	int     note;
	int     j;

	/* build table with 53 1/3 octaves, w/ MIDI range in center */

	/* normal halfstep is 12th of an even tempered octave */
	halfstep = exp(log(2.0) / 12.0);

	/* tuning works in 120th of a halfstep increments */
	tunestep = exp(log(2.0) / (12.0 * F_TUNING_RESOLUTION));

	/* for every patch tune adjustment */
	for (j = 0; j < 128; j++) {
		basefreq = a4freq * pow(tunestep, (double)(j - 64));

		/* build the table with a freshly calculated base freq every octave */
		for (octave = 0; octave < 54; octave++) {

			/* A4 down 27 octaves and 1 half-step */
			/* (table starts at 256 half-steps below midi range) */
			freq = basefreq * pow(2.0, (double)(octave - 27)) / halfstep;

			/* calculate notes for each octave with successive multiplications */
			for (note = octave * 12; note < (octave + 1) * 12; note++) {
				freq_table[j][note] = (sample_t) freq;
				freq                *= halfstep;
			}
		}
	}
}


/*****************************************************************************
 * build_freq_shift_table()
 *
 * Generates lookup table for exponential function used for
 * fine frequency adjustments.
 *****************************************************************************/
void
build_freq_shift_table(void)
{
	double  halfstep;
	double  tunestep;
	int     index;
	int     octaves;
	int     halfsteps;
	int     tunesteps;

	halfstep  = exp(log(2.0) / 12.0);
	tunestep = exp(log(2.0) / (12.0 * F_TUNING_RESOLUTION));

	for (index = 0; index < FREQ_SHIFT_TABLE_SIZE / 2; index++) {
		tunesteps = (FREQ_SHIFT_TABLE_SIZE / 2) - index;
		halfsteps = tunesteps / TUNING_RESOLUTION;
		tunesteps %= TUNING_RESOLUTION;
		octaves   = halfsteps / 12;
		halfsteps %= 12;
		freq_shift_table[index] = (sample_t)
			(1.0 /
			 (pow(2.0, (double) octaves) *
			  pow(halfstep, (double) halfsteps) *
			  pow(tunestep, (double) tunesteps)));
	}
	freq_shift_table[(FREQ_SHIFT_TABLE_SIZE / 2)] = 1.0;
	for (index = (FREQ_SHIFT_TABLE_SIZE / 2) + 1; index < FREQ_SHIFT_TABLE_SIZE; index++) {
		tunesteps = index - (FREQ_SHIFT_TABLE_SIZE / 2);
		halfsteps = tunesteps / TUNING_RESOLUTION;
		tunesteps %= TUNING_RESOLUTION;
		octaves   = halfsteps / 12;
		halfsteps %= 12;
		freq_shift_table[index] = (sample_t)
			(pow(2.0, (double) octaves) *
			 pow(halfstep, (double) halfsteps) *
			 pow(tunestep, (double) tunesteps));
	}
}


/*****************************************************************************
 * build_env_tables()
 *
 * Generates lookup tables for cc to envelope duration and
 * amplitude curve.
 *****************************************************************************/
void
build_env_tables(void)
{
	sample_t    env;
	sample_t    logval;
	sample_t    logstep;
	int         j;
	int         k;

	/* envelope duration table */
	for (j = 0; j < 128; j++) {
		env_table[j] = ((j + 0) * (j + 0) * (sample_rate / 6000)) + 4;

		env = 0.0;
		k = 0;
		while (env < 1.0) {
			env += (1.0 / (sample_t) env_table[j]);
			k++;
		}
		env_interval_dur[ENV_INTERVAL_ATTACK][j] = k;

		env = 1.0;
		k = 0;
		while (env > MINIMUM_GAIN) {
			env -= (1.0 / (sample_t)(2 * env_table[j]));
			k++;
		}
		env_interval_dur[ENV_INTERVAL_DECAY][j] = k;

		env_interval_dur[ENV_INTERVAL_SUSTAIN][j] = 1;

		env = 1.0;
		k = 0;
		while (env > MINIMUM_GAIN) {
			env -= (1.0 / (sample_t)(2 * env_table[j]));
			k++;
		}
		env_interval_dur[ENV_INTERVAL_RELEASE][j] = k + 60;

		env_interval_dur[ENV_INTERVAL_FADE][j] = -1;

		env_interval_dur[ENV_INTERVAL_DONE][j] = -1;
	}

	for (j = 0; j < 128; j++) {
		/* fade-out is fixed at short release */
		env_interval_dur[ENV_INTERVAL_FADE][j] =
			env_interval_dur[ENV_INTERVAL_RELEASE][19];
	}

	/* envelope logarithmic volume curve */
	logval  = (sample_t) log(F_ENV_CURVE_SIZE);
	logstep = 5.0 * F_ENV_CURVE_SIZE;
	env     = 1.0;
	for (j = ENV_CURVE_SIZE; j > (ENV_CURVE_SIZE / 2); j--) {
		env_curve[j] = (sample_t)(env);
		logstep      -= 5.0;
		env          /= (sample_t) exp(logval / (logstep));
	}
	for (j = (ENV_CURVE_SIZE / 2); j > 0; j--) {
		env_curve[j] = (sample_t)(env);
		logstep      -= 5.0;
		env          /= (sample_t) exp(logval / (logstep));
	}
	env_curve[0] = 0.0;
}


/*****************************************************************************
 * build_keyfollow_table()
 *
 * Generates lookup tables for cc to keyfollow volume adjustment
 *****************************************************************************/
void
build_keyfollow_table(void)
{
	int         ctlr;
	int         key;
	sample_t    slope;

	/* first the low freq weighted slope */
	for (ctlr = 0; ctlr < 64; ctlr++) {
		slope = (sample_t)(64 - ctlr) / 64.0;
		for (key = 0; key < 128; key++) {
			keyfollow_table[ctlr][key] =
				1.0 - (slope * (((sample_t)(key)) / 127.0));
		}
	}

	/* next the median */
	for (key = 0; key < 128; key++) {
		keyfollow_table[64][key] = 1.0;
	}

	/* last the high freq weighted slope */
	for (ctlr = 65; ctlr < 128; ctlr++) {
		slope = (sample_t)(ctlr - 64) / 63.0;
		for (key = 0; key < 128; key++) {
			keyfollow_table[ctlr][key] =
				1.0 - (slope * (((sample_t)(127 - key)) / 127.0));
		}
	}
}


/*****************************************************************************
 * build_mix_table()
 *
 * Generates lookup tables for cc to mix scalar mappings.
 *****************************************************************************/
void
build_mix_table(void)
{
	int     j;
	//sample_t  scale = 1.0 / (sqrtf (0.5));

	/* a simple parabolic curve works well, but still isn't perfect */
	for (j = 0; j < 128; j++) {
		mix_table[j] = ((sample_t) j) / 127.0;
		mix_table[j] += sqrtf(((float) j) / 127.0);
		mix_table[j] *= 0.5;
		//mix_table[j] = sqrtf ( ( (float) j) / 127.0) * scale;
	}
	mix_table[0]   = 0.0;
	mix_table[127] = 1.0;
}


/*****************************************************************************
 * build_pan_table()
 *
 * Generates lookup tables for cc to mix scalar mappings.
 *****************************************************************************/
void
build_pan_table(void)
{
	int         j;
	sample_t    scale = 1.0 / (sqrtf(0.5));

	for (j = 0; j < 128; j++) {
		pan_table[j] = sqrtf(((float) j) / 128.0) * scale;
	}
	//pan_table[0]   = 0.0;
	//pan_table[127] = 1.0;
}


/*****************************************************************************
 * build_gain_table()
 *
 * Generates lookup tables for cc to gain scalar mappings.
 *****************************************************************************/
void
build_gain_table(void)
{
	int         j;
	sample_t    gain;
	sample_t    step;

	/* 1/2 dB steps */
	step = expf(logf(10.0) / 40.0);

	gain = 1.0;
	for (j = 119; j >= 0; j--) {
		gain /= step;
		gain_table[j] = gain;
	}
	gain_table[120] = 1.0;
	gain = 1.0;
	for (j = 121; j < 128; j++) {
		gain *= step;
		gain_table[j] = gain;
	}
}


/*****************************************************************************
 * build_velocity_gain_table()
 *
 * Generates lookup tables for cc to gain scalar mappings.
 *****************************************************************************/
void
build_velocity_gain_table(void)
{
	int         j;
	int         k;
	sample_t    gain;
	sample_t    step;

	/* sensitivity 0 gets no gain adjustment */
	for (k = 0; k < 128; k++) {
		velocity_gain_table[0][k] = 1.0;
	}

	/* step through the sensitivity levels */
	for (j = 1; j < 128; j++) {

		/* tiny up to 1/2 dB steps */
		step = (sample_t) expf(logf(10.0) / (40 * (1.0 + ((127.0 - (float) j) / (float) j))));

		/* calculate gain for this sensitivity, centered at 100 */
		gain = 1.0;
		for (k = 99; k >= 0; k--) {
			gain /= step;
			velocity_gain_table[j][k] = gain;
		}
		velocity_gain_table[j][100] = 1.0;
		gain = 1.0;
		for (k = 101; k < 128; k++) {
			gain *= step;
			velocity_gain_table[j][k] = gain;
		}
	}

	for (j = 1; j < 128; j += 7) {
		for (k = 1; k < 128; k += 7) {
		}
	}
}


/*****************************************************************************
 * build_wave_tables()
 *
 * Generates high res wave tables for use by all oscillators and lfos
 *****************************************************************************/
void
build_waveform_tables(void)
{
	unsigned int    sample_num;
	unsigned int    wave_num;
	sample_t        phase;
	sample_t        opp_phase;
	sample_t        j, k, l, m;
	sample_t        wave_j;
	sample_t        wave_k;
	sample_t        wave_l;
	sample_t        wave_m;

	for (sample_num = 0; sample_num < WAVEFORM_SIZE; sample_num++) {

		phase = 2 * M_PI * (sample_t) sample_num / F_WAVEFORM_SIZE;
		opp_phase = phase + M_PI;

		wave_table[WAVE_SINE][sample_num]          = (sample_t) sin(phase);

		wave_table[WAVE_TRIANGLE][sample_num]      = func_triangle(sample_num);
		wave_table[WAVE_SAW][sample_num]           = func_saw(sample_num);
		wave_table[WAVE_REVSAW][sample_num]        = func_revsaw(sample_num);
		wave_table[WAVE_SQUARE][sample_num]        = func_square(sample_num);
		wave_table[WAVE_STAIR][sample_num]         = func_stair(sample_num);
		wave_table[WAVE_SAW_S][sample_num]         = func_saw(sample_num);
		wave_table[WAVE_REVSAW_S][sample_num]      = func_revsaw(sample_num);
		wave_table[WAVE_SQUARE_S][sample_num]      = func_square(sample_num);
		wave_table[WAVE_STAIR_S][sample_num]       = func_stair(sample_num);

		wave_table[WAVE_IDENTITY][sample_num]      = 1.0;
		wave_table[WAVE_NULL][sample_num]          = 0.0;

		wave_table[WAVE_POLY_SINE][sample_num]     = 0.0;
		wave_table[WAVE_POLY_SAW][sample_num]      = 0.0;
		wave_table[WAVE_POLY_REVSAW][sample_num]   = 0.0;
		wave_table[WAVE_POLY_SQUARE_1][sample_num] = 0.0;
		wave_table[WAVE_POLY_SQUARE_2][sample_num] = 0.0;
		wave_table[WAVE_POLY_1][sample_num]        = 0.0;
		wave_table[WAVE_POLY_2][sample_num]        = 0.0;
		wave_table[WAVE_POLY_3][sample_num]        = 0.0;
		wave_table[WAVE_POLY_4][sample_num]        = 0.0;

		j = 1.0;
		wave_j = (sample_t)(sinf((float) phase) * 1.00000 * j);

		wave_table[WAVE_POLY_REVSAW][sample_num]   += wave_j;
		wave_table[WAVE_POLY_1][sample_num]        += wave_j;
		wave_table[WAVE_POLY_2][sample_num]        += wave_j;
		wave_table[WAVE_POLY_3][sample_num]        += wave_j;

		wave_table[WAVE_POLY_SQUARE_1][sample_num] += wave_j;
		wave_table[WAVE_POLY_SINE][sample_num]     += wave_j;
		wave_table[WAVE_POLY_SQUARE_2][sample_num] += wave_j;
		wave_table[WAVE_POLY_4][sample_num]        += wave_j;

		wave_j = (sample_t)(sinf((float) opp_phase) * 1.00000 * j);
		wave_table[WAVE_POLY_SQUARE_1][sample_num] -= wave_j;
		wave_table[WAVE_POLY_SINE][sample_num]     -= wave_j;
		wave_table[WAVE_POLY_SQUARE_2][sample_num] -= wave_j;
		wave_table[WAVE_POLY_4][sample_num]        -= wave_j;

		for (j = 2.0; j <= 19.0; j += 1.0) {    /* j: even/odd harmonics    */
			k = ((j - 1.0) * 2.0);          /* k: even harmonics        */
			l = ((j - 1.0) * 2.0) + 1.0;    /* l: odd harmonics         */
			m = ((j - 1.0) * 3.0);          /* m: partial even/odd      */
			wave_j = (sample_t)(sinf((float)(j * phase)) * 1.00000 / j);
			wave_k = (sample_t)(sinf((float)(k * phase)) * 1.00000 / k);
			wave_l = (sample_t)(sinf((float)(l * phase)) * 1.00000 / l);
			wave_m = (sample_t)(sinf((float)(m * phase)) * 1.00000 / m);
			if (j <= 11.0) {
				wave_table[WAVE_POLY_REVSAW][sample_num] += wave_j;
				wave_table[WAVE_POLY_1][sample_num]      += wave_k;
				wave_table[WAVE_POLY_2][sample_num]      += wave_l;
				wave_table[WAVE_POLY_3][sample_num]      += wave_m;
			}
			wave_table[WAVE_POLY_SQUARE_1][sample_num] += wave_j;
			wave_table[WAVE_POLY_SINE][sample_num]     += wave_k;
			wave_table[WAVE_POLY_SQUARE_2][sample_num] += wave_l;
			wave_table[WAVE_POLY_4][sample_num]        += wave_m;

			wave_table[WAVE_POLY_SQUARE_1][sample_num] -=
				(sample_t)(sinf((float)(j * opp_phase)) * 1.00000 / j);
			wave_table[WAVE_POLY_SINE][sample_num]     -=
				(sample_t)(sinf((float)(k * opp_phase)) * 1.00000 / k);
			wave_table[WAVE_POLY_SQUARE_2][sample_num] -=
				(sample_t)(sinf((float)(l * opp_phase)) * 1.00000 / l);
			wave_table[WAVE_POLY_4][sample_num]        -=
				(sample_t)(sinf((float)(m * opp_phase)) * 1.00000 / m);
		}

		wave_table[WAVE_POLY_SAW][sample_num] =
			wave_table[WAVE_POLY_REVSAW][WAVEFORM_SIZE - sample_num];
	}

	/* bandlimit the _S (or BL) and Poly waveshapes */
	filter_wave_table_24dB(WAVE_SAW_S,         7, 8.0);
	filter_wave_table_24dB(WAVE_REVSAW_S,      7, 8.0);
	filter_wave_table_24dB(WAVE_SQUARE_S,      7, 8.0);
	filter_wave_table_24dB(WAVE_STAIR_S,       7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_SINE,     7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_SAW,      7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_REVSAW,   7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_SQUARE_1, 7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_SQUARE_2, 7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_1,        7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_2,        7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_3,        7, 8.0);
	filter_wave_table_24dB(WAVE_POLY_4,        7, 8.0);

	/* load and bandlimit sampled waveforms */
#ifdef ENABLE_SAMPLE_LOADING
	load_waveform_sample(WAVE_JUNO_OSC,       7, 6.0);
	load_waveform_sample(WAVE_JUNO_SAW,       7, 8.0);
	load_waveform_sample(WAVE_JUNO_SQUARE,    7, 8.0);
	load_waveform_sample(WAVE_JUNO_POLY,      7, 7.0);
	load_waveform_sample(WAVE_ANALOG_SQUARE,  7, 8.0);
	load_waveform_sample(WAVE_VOX_1,          7, 6.0);
	load_waveform_sample(WAVE_VOX_2,          7, 6.0);
#else
	for (sample_num = 0; sample_num < WAVEFORM_SIZE; sample_num++) {
		j = wave_table[WAVE_SINE][sample_num];

		wave_table[WAVE_JUNO_OSC][sample_num]       = j;
		wave_table[WAVE_JUNO_SAW][sample_num]       = j;
		wave_table[WAVE_JUNO_SQUARE][sample_num]    = j;
		wave_table[WAVE_JUNO_POLY][sample_num]      = j;
		wave_table[WAVE_ANALOG_SQUARE][sample_num]  = j;
		wave_table[WAVE_VOX_1][sample_num]          = j;
		wave_table[WAVE_VOX_2][sample_num]          = j;
	}
#endif

	/* wrap first four samples to end of each waveform table for hermite optimization */
	for (wave_num = 0; wave_num < NUM_WAVEFORMS; wave_num++) {
		for (sample_num = 0; sample_num < 4; sample_num++) {
			wave_table[wave_num][WAVEFORM_SIZE + sample_num] = wave_table[wave_num][sample_num];
		}
	}
}


/*****************************************************************************
 * load_waveform_sample()
 *
 * Loads sampled oscillator waveforms into the high-res wavetable.
 *****************************************************************************/
#ifdef ENABLE_SAMPLE_LOADING
int
load_waveform_sample(int wavenum, int num_cycles, double octaves)
{
	float           *in_buf;
	float           *out_buf;
	char            filename[PATH_MAX];
	struct stat     statbuf;
	SRC_DATA        src_data;
	FILE            *rawfile;
	unsigned int    sample;
	unsigned int    input_len;
	float           pos_max;
	float           neg_max;
	float           offset;
	float           scalar;
	size_t          sample_t_size = sizeof(sample_t);

	if ((in_buf = malloc(2 * WAVEFORM_SIZE * sizeof(float))) == NULL) {
		phasex_shutdown("Out of Memory!\n");
	}
	if ((out_buf = malloc(WAVEFORM_SIZE * sizeof(float))) == NULL) {
		phasex_shutdown("Out of Memory!\n");
	}

	/* get file size */
	snprintf(filename, PATH_MAX, "%s/%s.raw", SAMPLE_DIR, wave_names[wavenum]);
	if (stat(filename, &statbuf) != 0) {
		PHASEX_ERROR("Unable to load raw sample file '%s'\n", filename);
	}
	input_len = (unsigned int)((unsigned int) statbuf.st_size / sizeof(float));
	PHASEX_DEBUG(DEBUG_CLASS_INIT, "Loading raw waveform '%s': %d samples\n",
	             filename, input_len);

	/* open raw sample file */
	if ((rawfile = fopen(filename, "rb")) != 0) {

		/* read sample data */
		if (fread(in_buf, sizeof(float), input_len, rawfile) == input_len) {
			fclose(rawfile);

			/* normalize sample to [-1,1] */
			pos_max = neg_max = 0;
			for (sample = 0; sample < input_len; sample++) {
				if (in_buf[sample] > pos_max) {
					pos_max = in_buf[sample];
				}
				if (in_buf[sample] < neg_max) {
					neg_max = in_buf[sample];
				}
			}
			offset = (pos_max + neg_max) / -2.0;
			scalar = 1.0 / (float)((pos_max - neg_max) / 2.0);
			for (sample = 0; sample < input_len; sample++) {
				in_buf[sample] = (in_buf[sample] + offset) * scalar;
			}

			/* resample to fit WAVEFORM_SIZE */
			src_data.input_frames  = (long int) input_len;
			src_data.output_frames = WAVEFORM_SIZE;
			src_data.src_ratio     = F_WAVEFORM_SIZE / (double) input_len;
			src_data.data_in       = in_buf;
			src_data.data_out      =
				(sample_t_size == 4) ? (float *) & (wave_table[wavenum][0]) : & (out_buf[0]);

			src_simple(&src_data, SRC_SINC_BEST_QUALITY, 1);

			if (sample_t_size != 4) {
				for (sample = 0; sample < input_len; sample++) {
					wave_table[wavenum][sample] = (sample_t)(out_buf[sample]);
				}
			}

			/* filter resampled data */
			filter_wave_table_24dB(wavenum, num_cycles, octaves);

			/* all done */
			return 0;
		}
		fclose(rawfile);
	}

	/* copy sine data on failure */
	PHASEX_ERROR("Error reading '%s'!\n", filename);
	for (sample = 0; sample < WAVEFORM_SIZE; sample++) {
		wave_table[wavenum][sample] = wave_table[WAVE_SINE][sample];
	}

	return -1;
}
#endif /* ENABLE_SAMPLE_LOADING */


/*****************************************************************************
 * hermite()
 *
 * Read from a wavetable or sample buffer using hermite interpolation.
 *
 * TODO:  vector optimizations.
 *****************************************************************************/
#if NEED_GENERIC_HERMITE
sample_t
hermite(sample_t *buf, unsigned int max, sample_t index)
{
	sample_t        mu;
	sample_t        mu2;
	sample_t        mu3;
	sample_t        m0;
	sample_t        m1;
	sample_t        a0;
	sample_t        a1;
	sample_t        a2;
	sample_t        a3;
	sample_t        y0;
	sample_t        y1;
	sample_t        y2;
	sample_t        y3;
	sample_t        index_floor;
	unsigned int    index_int;

	/* integer value of index */
	index_floor = (sample_t) floorf((float) index);
	index_int = (((unsigned int) index_floor) + max + max - 1) % max;

	/* fractional portion of index */
	mu = index - index_floor;
	mu2 = mu * mu;
	mu3 = mu2 * mu;

	/* four adjacent samples, with higher precision index in the middle */
	y0 = buf[index_int];
	y1 = buf[(index_int + 1) % max];
	y2 = buf[(index_int + 2) % max];
	y3 = buf[(index_int + 3) % max];

	/* slope of first and second segments */
	m0  = ((y1 - y0 + y2 - y1) * 0.75);
	m1  = ((y2 - y1 + y3 - y2) * 0.75);

	/* setup the first part of the hermite polynomial */
	a0 = (2.0 * mu3) - (3.0 * mu2) + 1.0;
	a1 = (mu3) - (2.0 * mu2) + mu;
	a2 = (mu3) - (mu2);
	a3 = (-2.0 * mu3) + (3.0 * mu2);

	/* return the interpolated sample on the hermite curve */
	return ((a0 * y1) + (a1 * m0) + (a2 * m1) + (a3 * y2));
}
#endif /* NEED_GENERIC_HERMITE */


/*****************************************************************************
 * chorus_hermite()
 *
 * Read from a wavetable or sample buffer using hermite interpolation.
 *
 * TODO:  vector optimizations.
 *****************************************************************************/
sample_t
chorus_hermite(sample_t *buf, sample_t index)
{
	sample_t        mu;
	sample_t        mu2;
	sample_t        mu3;
	sample_t        m0;
	sample_t        m1;
	sample_t        a0;
	sample_t        a1;
	sample_t        a2;
	sample_t        a3;
	sample_t        y0;
	sample_t        y1;
	sample_t        y2;
	sample_t        y3;
	sample_t        index_floor;
	unsigned int    index_int;

	/* integer value of index */
	index_floor = (sample_t) floorf((float) index);
	index_int = (((unsigned int) index_floor) + CHORUS_MAX + CHORUS_MAX - 1) % CHORUS_MAX;

	/* fractional portion of index */
	mu = index - index_floor;
	mu2 = mu * mu;
	mu3 = mu2 * mu;

	/* four adjacent samples, with higher precision index in the middle */
	y0 = buf[index_int];
	y1 = buf[(index_int + 1) & CHORUS_MASK];
	y2 = buf[(index_int + 2) & CHORUS_MASK];
	y3 = buf[(index_int + 3) & CHORUS_MASK];

	/* slope of first and second segments */
	m0 = ((y1 - y0 + y2 - y1) * 0.75);
	m1 = ((y2 - y1 + y3 - y2) * 0.75);

	/* setup the first part of the hermite polynomial */
	a0 = (2.0 * mu3) - (3.0 * mu2) + 1.0;
	a1 = (mu3) - (2.0 * mu2) + mu;
	a2 = (mu3) - (mu2);
	a3 = (-2.0 * mu3) + (3.0 * mu2);

	/* return the interpolated sample on the hermite curve */
	return ((a0 * y1) + (a1 * m0) + (a2 * m1) + (a3 * y2));
}


/*****************************************************************************
 * wave_table_hermite()
 *
 * Read from oscillator wavetable using hermite interpolation.
 *
 * Tension and bias can be added with the following:
 *    m0  = ((y1 - y0) * (1.0 + bias) * (1.0 - tension) * 0.5) +
 *            ((y2 - y1) * (1.0 - bias) * (1.0 - tension) * 0.5);
 *    m1  = ((y2 - y1) * (1.0 + bias) * (1.0 - tension) * 0.5) +
 *            ((y3 - y2) * (1.0 - bias) * (1.0 - tension) * 0.5);
 *
 * TODO:  vector optimizations.
 *****************************************************************************/
sample_t
wave_table_hermite(int wave_num, sample_t index)
{
	sample_t        mu;
	sample_t        mu2;
	sample_t        mu3;
	sample_t        m0;
	sample_t        m1;
	sample_t        a0;
	sample_t        a1;
	sample_t        a2;
	sample_t        a3;
	sample_t        y0;
	sample_t        y1;
	sample_t        y2;
	sample_t        y3;
	sample_t        index_floor;
	unsigned int    index_int;

	/* integer value of index */
	index_floor = (sample_t) floorf((float) index);
	index_int = (((unsigned int) index_floor) + WAVEFORM_SIZE + WAVEFORM_SIZE - 1) % WAVEFORM_SIZE;

	/* fractional portion of index */
	mu = index - index_floor;
	mu2 = mu * mu;
	mu3 = mu2 * mu;

	/* Four adjacent samples, with higher precision index in the
	   middle first four samples of wavetables are duplicated at
	   the end to make this part modulus free. */
	y0 = wave_table[wave_num][index_int];
	y1 = wave_table[wave_num][index_int + 1];
	y2 = wave_table[wave_num][index_int + 2];
	y3 = wave_table[wave_num][index_int + 3];

	/* slope of first and second segments */
	m0 = ((y1 - y0 + y2 - y1) * 0.75);
	m1 = ((y2 - y1 + y3 - y2) * 0.75);

	/* setup the first part of the hermite polynomial */
	a0 = (2.0 * mu3) - (3.0 * mu2) + 1.0;
	a1 = (mu3) - (2.0 * mu2) + mu;
	a2 = (mu3) - (mu2);
	a3 = (-2.0 * mu3) + (3.0 * mu2);

	/* return the interpolated sample on the hermite curve */
	return ((a0 * y1) + (a1 * m0) + (a2 * m1) + (a3 * y2));
}


/*****************************************************************************
 * wave_table_linear()
 *
 * Read from oscillator wavetable using linear interpolation.
 *****************************************************************************/
sample_t
wave_table_linear(int wave_num, sample_t index)
{
	sample_t    y0;
	sample_t    y1;
	sample_t    index_floor;
	int         index_int;

	index_floor = (sample_t) floorf((float) index);
	index_int = (((int) index_floor) + WAVEFORM_SIZE + WAVEFORM_SIZE) % WAVEFORM_SIZE;

	y0 = wave_table[wave_num][index_int];
	y1 = wave_table[wave_num][index_int + 1];

	return (y0 + (y1 - y0) * (index - index_floor));
}


/*****************************************************************************
 *
 * Functions for the generating the waveform samples
 * The synth engine should never use these directly due to overhead.
 * Use the wave table instead!
 *
 * This should most definitely change in the future.  With today's fast
 * processors and relatively slower memory busses, optimized oscillator
 * callbacks should be much quicker than reading huge wavetables out of
 * RAM.  For true wavetable oscillators, small cachable wavetables and
 * optimized hermite interpolation shoud be faster than the current
 * memory lookups.
 *
 *****************************************************************************/


/*****************************************************************************
 * func_square()
 *
 * Square function for building wave table
 *****************************************************************************/
sample_t
func_square(unsigned int sample_num)
{
	if (sample_num < (WAVEFORM_SIZE / 2)) {
		return 1.0;
	}
	return -1.0;
}


/*****************************************************************************
 * func_saw()
 *
 * Saw function for building wave table
 *****************************************************************************/
sample_t
func_saw(unsigned int sample_num)
{
	return (2.0 * (sample_t) sample_num / F_WAVEFORM_SIZE) - 1.0;
}


/*****************************************************************************
 * func_revsaw()
 *
 * Reverse saw wave function for building wave table.
 *****************************************************************************/
sample_t
func_revsaw(unsigned int sample_num)
{
	return (2.0 * (F_WAVEFORM_SIZE - (sample_t) sample_num) / F_WAVEFORM_SIZE) - 1.0;
}


/*****************************************************************************
 * func_triangle()
 *
 * Triangle wave function for building wave table
 *****************************************************************************/
sample_t
func_triangle(unsigned int sample_num)
{
	if (sample_num < (WAVEFORM_SIZE / 4)) {
		return 4.0 * (sample_t) sample_num / F_WAVEFORM_SIZE;
	}
	sample_num -= (WAVEFORM_SIZE / 4);
	if (sample_num < (WAVEFORM_SIZE / 4)) {
		return 4.0 * ((F_WAVEFORM_SIZE / 4.0) - (sample_t) sample_num) / F_WAVEFORM_SIZE;
	}
	sample_num -= (WAVEFORM_SIZE / 4);
	if (sample_num < (WAVEFORM_SIZE / 4)) {
		return -4.0 * (sample_t) sample_num / F_WAVEFORM_SIZE;
	}
	sample_num -= (WAVEFORM_SIZE / 4);
	return -4.0 * ((F_WAVEFORM_SIZE / 4.0) - (sample_t) sample_num) / F_WAVEFORM_SIZE;
}


/*****************************************************************************
 * func_stair()
 *
 * Stairstep wave function for building wave table
 *****************************************************************************/
sample_t
func_stair(unsigned int sample_num)
{
	if (sample_num < (WAVEFORM_SIZE / 4)) {
		return 0.0;
	}
	sample_num -= (WAVEFORM_SIZE / 4);
	if (sample_num < (WAVEFORM_SIZE / 4)) {
		return 1.0;
	}
	sample_num -= (WAVEFORM_SIZE / 4);
	if (sample_num < (WAVEFORM_SIZE / 4)) {
		return 0.0;
	}
	return -1.0;
}
