/*****************************************************************************
 *
 * midimap.c
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
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include "phasex.h"
#include "config.h"
#include "param.h"
#include "bank.h"
#include "session.h"
#include "settings.h"
#include "engine.h"
#include "string_util.h"
#include "debug.h"


int         ccmatrix[128][16];

char        *midimap_filename   = NULL;
int         midimap_modified    = 0;


/*****************************************************************************
 * build_ccmatrix()
 *
 * Build the midi controller matrix from current param info.
 *****************************************************************************/
void
build_ccmatrix(void)
{
	PARAM_INFO      *param_info;
	int             cc;
	unsigned int    id;
	unsigned int    j;

	/* wipe it clean first */
	for (cc = 0; cc < 128; cc++) {
		for (j = 0; j < 16; j++) {
			ccmatrix[cc][j] = -1;
		}
	}

	/* now run through the params, one by one */
	for (id = 0; id < NUM_PARAMS; id++) {
		/* found a param that needs its cc num mapped */
		param_info = get_param_info_by_id(id);
		cc = param_info->cc_num;
		if ((cc  >= 0) && (cc < 128)) {
			/* find end of current list for cc num */
			j = 0;
			while ((ccmatrix[cc][j] >= 0) && (j < 16)) {
				j++;
			}
			/* map it after the first used slot */
			if (j < 16) {
				ccmatrix[cc][j] = (int) id;
			}
		}
	}
}


/*****************************************************************************
 * read_midimap()
 *****************************************************************************/
int
read_midimap(char *filename)
{
	PARAM           *param;
	char            buffer[256];
	char            param_name[32];
	FILE            *map_f;
	char            *p;
	char            c;
	int             id;
	unsigned int    part_num;
	unsigned int    param_num;
	int             cc_num;
	int             line = 0;

	/* open the midimap file */
	if ((map_f = fopen(filename, "rt")) == NULL) {
		return -1;
	}

	/* keep track of filename */
	if (filename != midimap_filename) {
		if (midimap_filename != NULL) {
			free(midimap_filename);
		}
		midimap_filename = strdup(filename);
	}

	/* read midimap entries */
	while (fgets(buffer, 256, map_f) != NULL) {
		line++;

		/* discard comments and blank lines */
		if ((buffer[0] == '\n') || (buffer[0] == '#')) {
			continue;
		}

		/* convert to lower case and strip comments for simpler parsing */
		p = buffer;
		while ((p < (buffer + 256)) && ((c = *p) != '\0')) {
			if (isupper(c)) {
				c  = (char) tolower(c);
				*p = c;
			}
			else if (c == '#') {
				*p = '\0';
			}
			p++;
		}

		/* get parameter name */
		if ((p = get_next_token(buffer)) == NULL) {
			while (get_next_token(buffer) != NULL);
			continue;
		}
		strncpy(param_name, p, sizeof(param_name));
		param_name[sizeof(param_name) - 1] = '\0';

		/* find named parameter */
		id = -1;
		for (param_num = 0; param_num < NUM_PARAMS; param_num++) {
			param = get_param(0, param_num);
			if (strcmp(param_name, param->info->name) == 0) {
				id = (int) param_num;
				break;
			}
		}

		/* make sure there's an '=' */
		if ((p = get_next_token(buffer)) == NULL) {
			while (get_next_token(buffer) != NULL);
			continue;
		}
		if (*p != '=') {
			while (get_next_token(buffer) != NULL);
			continue;
		}

		/* get midi cc num */
		if ((p = get_next_token(buffer)) == NULL) {
			while (get_next_token(buffer) != NULL);
			continue;
		}
		cc_num = atoi(p);
		if ((cc_num < -1) || (cc_num >= 128)) {
			cc_num = -1;
		}

		/* see if there's a ',locked' */
		if ((p = get_next_token(buffer)) == NULL) {
			while (get_next_token(buffer) != NULL);
			continue;
		}
		if (*p == ',') {
			if ((p = get_next_token(buffer)) == NULL) {
				while (get_next_token(buffer) != NULL);
				continue;
			}
			if (strcmp(p, "locked") != 0) {
				while (get_next_token(buffer) != NULL);
				continue;
			}
			if ((id >= 0) && (id < NUM_PARAMS)) {
				for (part_num = 0; part_num < MAX_PARTS; part_num++) {
					param = get_param(part_num, (unsigned int) id);
					param->info->locked = 1;
				}
			}
			if ((p = get_next_token(buffer)) == NULL) {
				while (get_next_token(buffer) != NULL);
				continue;
			}
		}
		else {
			if ((id >= 0) && (id < NUM_PARAMS)) {
				for (part_num = 0; part_num < MAX_PARTS; part_num++) {
					param = get_param(part_num, (unsigned int) id);
					param->info->locked = 0;
				}
			}
		}

		/* make sure there's a ';' */
		if (*p != ';') {
			while (get_next_token(buffer) != NULL);
			continue;
		}

		/* flush remainder of line */
		while (get_next_token(buffer) != NULL);

		/* set midi cc number */
		if ((id >= 0) && (id < NUM_PARAMS)) {
			for (part_num = 0; part_num < MAX_PARTS; part_num++) {
				param = get_param(part_num, (unsigned int) id);
				PHASEX_DEBUG(DEBUG_CLASS_GUI, "Load MIDI Map: id=[%03d] cc=[%03d] param=<%s>\n",
				             id, cc_num, param->info->name);
				param->info->cc_num = cc_num;
			}
		}
		else if (debug && (strcmp(param_name, "midi_channel") != 0)) {
			PHASEX_WARN("Unknown parameter '%s' in midimap '%s', line %d.\n",
			            param_name, midimap_filename, line);
		}
	}

	/* done parsing */
	fclose(map_f);
	midimap_modified = 0;

	/* rebuild ccmatrix based on new map */
	build_ccmatrix();

	return 0;
}


/*****************************************************************************
 * save_midimap()
 *****************************************************************************/
int
save_midimap(char *filename)
{
	PARAM           *param;
	FILE            *map_f;
	unsigned int    param_num;
	unsigned int    k;

	/* open the midimap file */
	if ((map_f = fopen(filename, "wt")) == NULL) {
		PHASEX_ERROR("Error opening midimap file %s for write: %s\n",
		             filename, strerror(errno));
		return -1;
	}

	/* keep track of filename */
	if (filename != midimap_filename) {
		if (midimap_filename != NULL) {
			free(midimap_filename);
		}
		midimap_filename = strdup(filename);
	}

	/* output 'param_name = cc_num;' for each param */
	for (param_num = 0; param_num < NUM_PARAMS; param_num++) {
		param = get_param(0, param_num);
		fprintf(map_f, "%s", param->info->name);
		k = 32 - (unsigned int) strlen(param->info->name);
		while (k > 8) {
			fputc('\t', map_f);
			k -= 8;
		}
		fprintf(map_f, "= %d%s;\n",
		        param->info->cc_num,
		        (param->info->locked ? ",locked" : ""));
	}

	/* done writing file */
	fclose(map_f);
	midimap_modified = 0;

	return 0;
}
