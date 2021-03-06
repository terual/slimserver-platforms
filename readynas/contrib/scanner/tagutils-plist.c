//=========================================================================
// FILENAME	: playlist.c
// DESCRIPTION	: Playlist
//=========================================================================
// Copyright (c) 2008- NETGEAR, Inc. All Rights Reserved.
//=========================================================================

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "misc.h"
#include "tagutils.h"
#include "textutils.h"
#include "log.h"


#define MAX_BUF 1024

static FILE *fp = 0;
static int _utf8bom = 0;

static int (*_next_track)(struct song_metadata*, struct stat*, char*, char*);
static int _m3u_next_track(struct song_metadata*, struct stat*, char*, char*);

int
start_plist(char *path, struct song_metadata *psong, struct stat *stat, char *lang, char *type)
{
  char *fname, *suffix;
  _next_track = 0;
  _utf8bom = 0;

  if (!strcmp(type, "m3u"))
    _next_track = _m3u_next_track;

  if (!_next_track) {
    DPRINTF(E_ERROR, L_SCAN_SCANNER, "Non supported type of playlist <%s>\n", type);
    return -1;
  }

  if (!(fp = fopen(path, "rb"))) {
    DPRINTF(E_ERROR, L_SCAN_SCANNER, "Cannot open %s\n", path);
    return -1;
  }

  //
  memset((void*)psong, 0, sizeof(struct song_metadata));
  psong->is_plist = 1;
  psong->path = strdup(path);
  psong->type = type;

  fname = strrchr(psong->path, '/');
  psong->basename = fname ? fname+1 : psong->path;

  psong->title = strdup(psong->basename);
  suffix = strrchr(psong->title, '.');
  if (suffix) *suffix = '\0';
  psong->titlesearch = psong->titlesort = canonicalize_name(psong->title);

  if (stat) {
    if (!psong->time_modified)
      psong->time_modified = stat->st_mtime;
    psong->file_size = stat->st_size;
  }

  return 0;
}

int
_m3u_next_track(struct song_metadata *psong, struct stat *stat, char *lang, char *type)
{
  char buf[MAX_BUF], *p;
  int len;

  //
  memset((void*)psong, 0, sizeof(struct song_metadata));

  // read first line, check BOM
  p = fgets(buf, MAX_BUF, fp);
  if (!p) {
    fclose(fp);
    return 1;
  }

  if (!_utf8bom && p[0]=='\xef' && p[1]=='\xbb' && p[2]=='\xbf') {
    _utf8bom = 1;
    p += 3;
  }

  while (p) {
    while (isspace(*p)) p++;
    if (*p && *p!='#') {
      // check dos format
      len = strlen((char*)p);

      while (p[len-1]=='\r' || p[len-1]=='\n') {
	p[--len] = '\0';
      }
      psong->path = strdup(p);
      return 0;
    }
    p = fgets(buf, MAX_BUF, fp);
    continue;
  }

  fclose(fp);
  return -1;
}

int
next_plist_track(struct song_metadata *psong, struct stat *stat, char *lang, char *type)
{
  if (_next_track)
    return _next_track(psong, stat, lang, type);
  return -1;
}
