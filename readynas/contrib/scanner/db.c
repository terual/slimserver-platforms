//=========================================================================
// FILENAME	: db.c
// DESCRIPTION	: Database interface for scanner
//=========================================================================
// Copyright (c) 2008- NETGEAR, Inc. All Rights Reserved.
//=========================================================================

/* This program is free software; you can redistribute it and/or modify
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

#include <mysql/mysql.h>
#include <mysql/mysqld_error.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scanner.h"
#include "tagutils.h"
#include "log.h"
#include "sqlprintf.h"
#include "db.h"
#include "textutils.h"
#include "artwork.h"

struct _cache {
  int various_artist;
} cache = {
  .various_artist = 0
};

static char qstr[2048];

static int
_db_query(MYSQL *mysql, char *qstr, int ignore_errno)
{
  int err;
  DPRINTF(E_INFO, L_DB_SQL, "SQL=%s\n", qstr);
  err = mysql_query(mysql, qstr);

  if ((!err) || (ignore_errno && mysql_errno(mysql)==ignore_errno))
      return 0;

  DPRINTF(E_ERROR, L_DB_MYSQL, "mysql_error: %s on SQL=<%s>\n", mysql_error(mysql), qstr);
  return err;
}

static char*
_song_length_str(int len)
{
  static char str[16];
  if (len<=0) {
    strcpy(str, "NULL");
  }
  else {
    snprintf(str, sizeof(str)-1, "%d.%03d",
	     len/1000, len%1000);
  }
  return str;
}


static int
_get_various_artist(int *id, MYSQL *mysql)
{
  char *str_va = "Various Artist";		// TBD: This should read from string file
  char *str_va_canonicalized = canonicalize_name(str_va);
  char *p;
  size_t room;
  int n, err;
  MYSQL_RES *result = 0;
  MYSQL_ROW row;

  // check if record already exist
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) sql_snprintf(p, room, "select id from contributors where name='%S'", str_va);
  if ((err = _db_query(mysql, qstr, 0)))
    goto _exit;
  if (!(result = mysql_store_result(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "No return result on select\n");
    err = -1;
    goto _exit;
  }
  if ((mysql_num_fields(result))) {
    if ((row = mysql_fetch_row(result))) {
      // exist, get id
      *id = atoi(row[0]);
    }
    else {
      // not exist, insert it
      p = qstr;
      room = sizeof(qstr) - 1;
      n = snprintf(p, room, "insert into contributors (name,namesort,namesearch) values ");
      p += n; room -= n;
      sql_snprintf(p, room, "('%S','%S','%S')",
		      str_va, str_va_canonicalized, str_va_canonicalized);
      if ((err = _db_query(mysql, qstr, 0)))
	goto _exit;
      *id = mysql_insert_id(mysql);
    }
  }
  else {
    DPRINTF(E_INFO, L_DB_SQL, "Unexpected error\n");
    err = -1;
    goto _exit;
  }

 _exit:
  if (result)
    mysql_free_result(result);
  if (str_va_canonicalized)
    free(str_va_canonicalized);

  return err;
}

static int
_insert_contributor_album_track(MYSQL *mysql, struct song_metadata *psong, int role)
{
  char *p;
  size_t room;
  int err;

  if (psong->album) {
    p = qstr;
    room = sizeof(qstr) - 1;
    (void) snprintf(p, room, "insert ignore into contributor_album "
		    "(role,contributor,album) values (%d,%ld,%ld)",
		    role, psong->contributor_id[role], psong->album_id);
    if ((err = _db_query(mysql, qstr, 0)))
      return err;
  }
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) snprintf(p, room, "insert into contributor_track "
		  "(role,contributor,track) values (%d,%ld,%ld)",
		  role, psong->contributor_id[role], psong->track_id);
  if ((err = _db_query(mysql, qstr, 0)))
    return err;

  return 0;
}

static int
_insert_contributor(MYSQL *mysql, struct song_metadata *psong, int role)
{
  char *p;
  size_t room;
  int n, err;
  MYSQL_RES *result;
  MYSQL_ROW row;

  // check if record already exist
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) sql_snprintf(p, room, "select id from contributors where name='%S'", psong->contributor[role]);
  if ((err = _db_query(mysql, qstr, 0)))
    return err;
  if (!(result = mysql_store_result(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "No return result on select\n");
    return -1;
  }
  if ((mysql_num_fields(result))) {
    if ((row = mysql_fetch_row(result))) {
      // exist, get id
      psong->contributor_id[role] = atoi(row[0]);
    }
    else {
      // not exist, insert it
      p = qstr;
      room = sizeof(qstr) - 1;
      n = snprintf(p, room, "insert into contributors (name,namesort,namesearch) values ");
      p += n; room -= n;
      sql_snprintf(p, room, "('%S','%S','%S')",
		   psong->contributor[role], psong->contributor[role],  psong->contributor[role]);
      if ((err = _db_query(mysql, qstr, 0)))
	return err;
      psong->contributor_id[role] = mysql_insert_id(mysql);
    }
  }
  else {
    DPRINTF(E_INFO, L_DB_SQL, "Unexpected error\n");
    mysql_free_result(result);
    return -1;
  }

  mysql_free_result(result);

  return 0;
}

static int
_insert_tracks(MYSQL *mysql, struct song_metadata *psong)
{
  char *p;
  size_t room;
  int n, err;

  p = qstr;
  room = sizeof(qstr) - 1;
  if (psong->track_id) {
    n = snprintf(p, room, "insert into tracks (id,");
  }
  else {
    n = snprintf(p, room, "insert into tracks (");
  }
  p += n; room -= n;
  n = snprintf(p, room,
	       "url,title,titlesort,titlesearch,"
	       "album,tracknum,content_type,timestamp,filesize,"
	       "audio_size,audio_offset,year,secs,"
	       "vbr_scale,bitrate,samplerate,samplesize,channels,block_alignment,"
	       "bpm,tagversion,drm,rating,"
	       "disc,audio,remote,lossless"
	       ") values (");
  p += n; room -= n;
  if (psong->track_id) {
    n = snprintf(p, room, "%lu,", psong->track_id);
    p += n; room -= n;
  }
  sql_snprintf(p, room, "'file://%U','%S','%S','%S',"
	       "%I,%I,'%S',%u,%u,"		// album, ...
	       "%I,%d,%d,%S,"			// audio_size, ...
	       "%D,%D,%D,%I,%I,%I,"		// vbr_scale, ...
	       "%I,%T,%d,%d,"			// bpm, ...
	       "%I,%d,%d,%d)",			// disc ...
	       psong->path,			// url
	       psong->title,
	       psong->titlesort,
	       psong->titlesearch,
	       psong->album_id,			// album
	       psong->track,
	       psong->sstype,
	       (unsigned int) psong->time_modified,
	       (unsigned int) psong->file_size,
	       psong->audio_size,		// audio_size
	       psong->audio_offset,
	       psong->year,
	       _song_length_str(psong->song_length),
	       psong->vbr_scale,		// vbr_scale
	       psong->bitrate,
	       psong->samplerate,
	       psong->samplesize,
	       psong->channels,
	       psong->blockalignment,
	       psong->bpm,			// bpm
	       psong->tagversion,
	       0,
	       0,
	       psong->disc,			// disc
	       1,
	       0,
	       psong->lossless
	       );
  if ((err = _db_query(mysql, qstr, 0)))
    return err;
  psong->track_id = mysql_insert_id(mysql);
  return 0;
}

static int
_insert_comment(MYSQL *mysql, struct song_metadata *psong)
{
  char *p;
  size_t room;
  int n;

  p = qstr;
  room = sizeof(qstr) - 1;
  n = snprintf(p, room, "insert into comments (track,value) values ");
  p += n; room -= n;
  sql_snprintf(p, room, "(%d,'%S')",
	       psong->track_id, psong->comment);
  return _db_query(mysql, qstr, ER_DUP_ENTRY);
}

static int
_insert_genre(MYSQL *mysql, struct song_metadata *psong)
{
  char *p;
  size_t room;
  int n, err = 0;
  MYSQL_RES *result;
  MYSQL_ROW row;

  p = qstr;
  room = sizeof(qstr) - 1;
  (void) sql_snprintf(p, room, "select id from genres where name='%S'", psong->genre);
  if ((err = _db_query(mysql, qstr, 0)))
    return err;
  if (!(result = mysql_store_result(mysql))) {
    DPRINTF(E_INFO, L_DB_SQL, "Internal Error%s\n");
    return -1;
  }
  if ((mysql_num_fields(result))) {
    if ((row = mysql_fetch_row(result))) {
      // exist, get id
      psong->genre_id = atoi(row[0]);
    }
    else {
      // not exist, insert it
      char *canonicalized_genre = canonicalize_name(psong->genre);
      p = qstr;
      room = sizeof(qstr) - 1;
      n = sql_snprintf(p, room, "insert into genres (name,namesort,namesearch) values");
      p += n; room -= n;
      sql_snprintf(p, room, "('%S','%S','%S')",
		   psong->genre, canonicalized_genre, canonicalized_genre);
      if (canonicalized_genre != psong->genre)
	free(canonicalized_genre);
      if ((err = _db_query(mysql, qstr, 0))) {
	mysql_free_result(result);
	return err;
      }
      psong->genre_id = mysql_insert_id(mysql);
    }
  }
  mysql_free_result(result);
  return 0;
}

static int
_insert_album(MYSQL *mysql, struct song_metadata *psong)
{
  char *p;
  size_t room;
  int n, err = 0;
  MYSQL_RES *result;
  MYSQL_ROW row;

  // check if record already exist.
  p = qstr;
  room = sizeof(qstr) - 1;
  n = sql_snprintf(p, room, "select me.id,me.compilation,me.contributor from albums me "
		   "join tracks on (me.id=tracks.album) "
		   "where me.title='%S' and tracks.url like 'file://%K/%%' and",
		   psong->album, psong->dirpath);
  p += n; room -= n;
  if (psong->disc)
    n = snprintf(p, room, " me.disc=%d and", psong->disc);
  else
    n = snprintf(p, room, " me.disc is null and");
  p += n; room -= n;
  if (psong->total_discs)
    n = snprintf(p, room, " me.discc=%d", psong->total_discs);
  else
    n = snprintf(p, room, " me.discc is null");

  if ((err = _db_query(mysql, qstr, 0)))
    return err;
  if (!(result = mysql_store_result(mysql))) {
    DPRINTF(E_INFO, L_DB_SQL, "Internal Error%s\n");
    return -1;
  }

  if ((mysql_num_fields(result))) {
    if ((row = mysql_fetch_row(result))) {
      // exist, get id
      psong->album_id = atoi(row[0]);
      // set compilation flag, if not set yet AND different contributor
      if (!safe_atoi(row[1]) &&
	  safe_atoi(row[2]) != psong->contributor_id[ROLE_ALBUMARTIST]) {
	if (!cache.various_artist) {
	  if ((err = _get_various_artist(&cache.various_artist, mysql))) {
	    mysql_free_result(result);
	    return err;
	  }
	}

	p = qstr;
	room = sizeof(qstr) - 1;
	(void) sql_snprintf(p, room, "update albums set compilation=1,contributor=%d where id=%d",
			    cache.various_artist, psong->album_id);
	if ((err = _db_query(mysql, qstr, 0))) {
	  mysql_free_result(result);
	  return err;
	}
      }
    }
    else {
      // not exist, insert it
      char *canonicalized_album;
      p = qstr;
      room = sizeof(qstr) - 1;
      n = snprintf(p, room, "insert into albums ("
		   "title,titlesort,titlesearch,"
		   "compilation,year,"
		   "disc,discc,contributor"
		   ") values ");
      p += n; room -= n;
      canonicalized_album = canonicalize_name(psong->album);
      sql_snprintf(p, room,
		   "('%S','%S','%S',"		// album
		   "%I,%d,"			// compilation
		  " %I,%I,%d)",			// disc
		   psong->album, canonicalized_album, canonicalized_album,
		   psong->compilation, psong->year,
		   psong->disc, psong->total_discs, psong->contributor_id[ROLE_ALBUMARTIST]
		   );
      if (canonicalized_album != psong->album)
	free(canonicalized_album);
      if ((err = _db_query(mysql, qstr, 0))) {
	mysql_free_result(result);
	return err;
      }
      psong->album_id = mysql_insert_id(mysql);
    }
  }
  else {
    DPRINTF(E_INFO, L_DB_SQL, "Unexpected error\n");
    mysql_free_result(result);
    return -1;
  }
  mysql_free_result(result);
  return 0;
}

static int
_insert_year(MYSQL *mysql, struct song_metadata *psong)
{
  snprintf(qstr, sizeof(qstr), "insert ignore into years (id) values (%d)", psong->year);
  return _db_query(mysql, qstr, 0);
}

static int
_insert_genre_track(MYSQL *mysql, struct song_metadata *psong)
{
  snprintf(qstr, sizeof(qstr), "insert into genre_track (genre,track) values"
	   "(%lu,%lu)", psong->genre_id, psong->track_id);
  return _db_query(mysql, qstr, 0);
}

static int
_get_track_id(MYSQL *mysql, struct song_metadata *psong)
{
  char *p;
  size_t room;
  int err;
  MYSQL_RES *result;
  MYSQL_ROW row;

  p = qstr;
  room = sizeof(qstr) - 1;
  (void) sql_snprintf(p, room, "select id from tracks where url='file://%U'", psong->path);
  if ((err = _db_query(mysql, qstr, 0)))
    return err;
  if (!(result = mysql_store_result(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "No return result on select\n");
    return -1;
  }
  if ((mysql_num_fields(result))) {
    if ((row = mysql_fetch_row(result))) {
      // exist, get timestamp
      psong->track_id = strtoul(row[0], 0, 10);
    }
  }
  mysql_free_result(result);
  return 0;
}

static int
_delete_track_by_id(MYSQL *mysql, struct song_metadata *psong)
{
  char *p;
  size_t room;

  p = qstr;
  room = sizeof(qstr) - 1;
  (void) snprintf(p, room, "delete from tracks where id=%lu", psong->track_id);
  if (_db_query(mysql, qstr, 0))
    return -1;

  return 0;
}

int
_insertdb_song(MYSQL *mysql, struct song_metadata *psong)
{
  int role, err;
  int update = 0;

  // disable auto commit
  if ((err = mysql_autocommit(mysql, 0))) {
    DPRINTF(E_INFO, L_DB_SQL, "autocommit=0: %s\n", mysql_error(mysql));
    return err;
  }

  // years
  if ((err = _insert_year(mysql, psong)))
    return err;

  // genre
  if (psong->genre) {
    if ((err = _insert_genre(mysql, psong)))
      return err;
  }

  // contributors
  for (role=0; role<(N_ROLE-1); role++) {
    if (psong->contributor[role]) {
      if ((err = _insert_contributor(mysql, psong, role)))
	return err;
    }
  }

  psong->contributor_id[ROLE_ALBUMARTIST] = psong->contributor_id[ROLE_ARTIST];

  // albums
  if (psong->album) {
    if ((err = _insert_album(mysql, psong)))
      return err;
  }

  // tracks
  if (!G.wipe && !_get_track_id(mysql, psong)) {
    // track exist
    _delete_track_by_id(mysql, psong);
    update = 1;
  }
  if ((err = _insert_tracks(mysql, psong)))
    return err;

  // contributor_album, contributor_track
  for (role=1; role<(N_ROLE-1); role++) {
    if (psong->contributor[role]) {
      if ((err =_insert_contributor_album_track(mysql, psong, role)))
	return err;
    }
  }

  // genre_track
  if (psong->genre_id) {
    if ((err = _insert_genre_track(mysql, psong)))
      return err;
  }


  // comments
  if (psong->comment) {
    if ((err = _insert_comment(mysql, psong)))
      return err;
  }


  // --- commit ---
  if ((err = mysql_commit(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "commit: %s\n", mysql_error(mysql));
    return err;
  }

  if (update)
    G.updated_songs++;
  else
    G.added_songs++;

  return 0;
}

static int
_insertdb_plist(MYSQL *mysql, struct song_metadata *psong)
{
  char *p;
  size_t room;
  int n, err;
  int update = 0;
  unsigned long track_id=0;

  // disable auto commit
  if ((err = mysql_autocommit(mysql, 0))) {
    DPRINTF(E_INFO, L_DB_SQL, "autocommit=0: %s\n", mysql_error(mysql));
    return -1;
  }

  // track
  if (!G.wipe && !_get_track_id(mysql, psong)) {
    // track exist. (track is playlist, 'ssp')
    _delete_track_by_id(mysql, psong);
    update = 1;
  }
  p = qstr;
  room = sizeof(qstr) - 1;
  if (update) {
    n = snprintf(p, room, "insert into tracks (id,");
  }
  else {
    n = snprintf(p, room, "insert into tracks (");
  }
  p += n; room -= n;
  n = snprintf(p, room,
	       "url,title,titlesort,titlesearch,"
	       "content_type,timestamp,filesize,"
	       "rating,remote,musicmagic_mixable"
	       ") values (");
  p += n; room -= n;
  if (update) {
    n = snprintf(p, room, "%lu,", track_id);
    p += n; room -= n;
  }
  sql_snprintf(p, room,
	       "'file://%U','%S','%S','%S',"	// url, ...
	       "'%S',%u,%d,"			// content_type, ...
	       "%d,%d,%d"			// raiting
	       ")",
	       psong->path,			// url
	       psong->title,
	       psong->titlesort,
	       psong->titlesearch,
	       "ssp",				// content_type
	       psong->time_modified,
	       psong->file_size,
	       0,				// rating
	       0,
	       1
	       );
  if ((err = _db_query(mysql, qstr, 0))) {
    return -1;
  }
  track_id = mysql_insert_id(mysql);

  // --- commit ---
  if ((err = mysql_commit(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "commit: %s\n", mysql_error(mysql));
    return -1;
  }

  psong->plist_id = track_id;

  return 0;
}

int
_insertdb_plist_item(MYSQL *mysql, struct song_metadata *psong)
{
  int err;

  // disable auto commit
  if ((err = mysql_autocommit(mysql, 0))) {
    DPRINTF(E_INFO, L_DB_SQL, "autocommit=0: %s\n", mysql_error(mysql));
    return err;
  }

  // tracks
  if (_get_track_id(mysql, psong)) {
    // track not exist, then insert it
    if ((err = _insert_tracks(mysql, psong)))
      return err;
    psong->track_id = mysql_insert_id(mysql);
  }

  // playlist_track
  snprintf(qstr, sizeof(qstr),
	   "insert into playlist_track (position,playlist,track) "
	   "values (%d,%d,%lu)",
	   psong->plist_position, psong->plist_id, psong->track_id);
  if ((err = _db_query(mysql, qstr, 0)))
    return err;

  // --- commit ---
  if ((err = mysql_commit(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "commit: %s\n", mysql_error(mysql));
    return err;
  }

  return 0;
}

int
db_set_lastrescantime(MYSQL *mysql)
{
  char *p;
  size_t room;
  int err;

  // delete
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) sql_snprintf(p, room, "delete from metainformation where name='lastRescanTime'");
  if ((err = _db_query(mysql, qstr, 0)))
    return err;

  // then insert
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) snprintf(p, room, "insert into metainformation (name,value) values "
		  "('lastRescanTime','%llu')", (unsigned long long) time(0));
  if ((err = _db_query(mysql, qstr, 0)))
    return err;

  // commit
  if ((err = mysql_commit(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "commit: %s\n", mysql_error(mysql));
    return err;
  }

  return 0;
}

int
db_get_lastrescantime(MYSQL *mysql, time_t *lastrescantime)
{
  char *p;
  size_t room;
  int err;
  MYSQL_RES *result;
  MYSQL_ROW row;

  *lastrescantime = 0;

  // delete
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) sql_snprintf(p, room, "select value from metainformation where name='lastRescanTime'");
  if ((err = _db_query(mysql, qstr, 0)))
    return err;
  if (!(result = mysql_store_result(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "No return result on select\n");
    return -1;
  }
  if ((mysql_num_fields(result))) {
    if ((row = mysql_fetch_row(result))) {
      // exist, get it
      *lastrescantime = strtoul(row[0], 0, 10);
    }
  }
  mysql_free_result(result);

  return 0;
}

int
db_set_scanning(MYSQL *mysql, int f)
{
  char *p;
  size_t room;
  int err;

  // delete
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) sql_snprintf(p, room, "delete from metainformation where name='isScanning'");
  if ((err = _db_query(mysql, qstr, 0)))
    return err;

  // then insert
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) snprintf(p, room, "insert into metainformation (name,value) values "
		  "('isScanning','%d')", f);
  if ((err = _db_query(mysql, qstr, 0)))
    return err;

  // commit
  if ((err = mysql_commit(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "commit: %s\n", mysql_error(mysql));
    return err;
  }

  return 0;
}

int
db_get_scanning(MYSQL *mysql, int *scanning)
{
  char *p;
  size_t room;
  int err;
  MYSQL_RES *result;
  MYSQL_ROW row;

  *scanning = 0;

  // delete
  p = qstr;
  room = sizeof(qstr) - 1;
  (void) sql_snprintf(p, room, "select value from metainformation where name='scanning'");
  if ((err = _db_query(mysql, qstr, 0)))
    return err;
  if (!(result = mysql_store_result(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "No return result on select\n");
    return -1;
  }
  if ((mysql_num_fields(result))) {
    if ((row = mysql_fetch_row(result))) {
      // exist, get ii
      *scanning = atoi(row[0]);
    }
  }
  mysql_free_result(result);

  return 0;
}

char *tables[] = {
  "tracks",
  "playlist_track",
  "albums",
  "years",
  "contributors",
  "contributor_track",
  "contributor_album",
  "genres",
  "genre_track",
  "comments",
  "pluginversion",
  "unreadable_tracks",
  0
};

int
db_wipe(MYSQL *mysql)
{
  int err;
  int i;

  // enable auto commit
  if ((err = mysql_autocommit(mysql, 1))) {
    DPRINTF(E_INFO, L_DB_SQL, "autocommit=1: %s\n", mysql_error(mysql));
    return err;
  }

  _db_query(mysql, "set foreign_key_checks = 0", 0);
  for (i=0; tables[i]; i++) {
    (void) snprintf(qstr, sizeof(qstr), "delete from %s", tables[i]);
    _db_query(mysql, qstr, 0);
    (void) snprintf(qstr, sizeof(qstr), "alter table %s AUTO_INCREMENT=1", tables[i]);
    _db_query(mysql, qstr, 0);
  }
  _db_query(mysql, "update metainformation set value = 0 where name = 'lastRescanTime'", 0);
  _db_query(mysql, "set foreign_key_checks = 1", 0);


  if (0) {
    for (i=0; tables[i]; i++) {
      (void) snprintf(qstr, sizeof(qstr), "optimize table %s", tables[i]);
      _db_query(mysql, qstr, 0);
    }
  }

  return 0;
}

int
db_merge_artists_albums(MYSQL *mysql)
{
  return 0;
}

int
db_find_artworks(MYSQL *mysql)
{
  int err;
  MYSQL_RES *result;
  MYSQL_ROW row;
  int album_id;
  int track_id;
  char *cover_file;
  char *track_url;

  // disable auto commit
  if ((err = mysql_autocommit(mysql, 0))) {
    DPRINTF(E_INFO, L_DB_SQL, "autocommit=0: %s\n", mysql_error(mysql));
    return err;
  }

  // find album without artwork
  if ((err = _db_query(mysql,
		       "SELECT me.id,cover,albums.id,url FROM tracks me"
		       " JOIN albums ON (albums.id = me.album)"
		       " WHERE (albums.artwork IS NULL) GROUP BY album", 0)))
    return err;
  if (!(result = mysql_store_result(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "No return result on select in db_find_artworks()\n");
    return -1;
  }
  if (!(mysql_num_fields(result))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "Unexpected error in db_find_artworks()\n");
    mysql_free_result(result);
    return -1;
  }
  while ((row = mysql_fetch_row(result))) {
    track_id = atoi(row[0]);
    cover_file = row[1];
    album_id = atoi(row[2]);
    track_url = row[3];
    if (cover_file) {
      (void) snprintf(qstr, sizeof(qstr),
		      "UPDATE albums set artwork=%d WHERE id=%d", track_id, album_id);
      if ((err = _db_query(mysql, qstr, 0))) {
	mysql_free_result(result);
	return -1;
      }
    }
    else {
      if ((cover_file = artwork_find_file(track_url))) {
	(void) sql_snprintf(qstr, sizeof(qstr),
			   "UPDATE tracks set cover='%S' WHERE id=%d", cover_file, track_id);
	if ((err = _db_query(mysql, qstr, 0))) {
	  free(cover_file);
	  mysql_free_result(result);
	  return -1;
	}
	(void) snprintf(qstr, sizeof(qstr),
			"UPDATE albums set artwork=%d WHERE id=%d", track_id, album_id);
	if ((err = _db_query(mysql, qstr, 0))) {
	  free(cover_file);
	  mysql_free_result(result);
	  return -1;
	}
	free(cover_file);
      }
    }
  }

  mysql_free_result(result);

  // commit
  if ((err = mysql_commit(mysql))) {
    DPRINTF(E_DEBUG, L_DB_MYSQL, "commit: %s\n", mysql_error(mysql));
    return err;
  }

  return 0;
}

int
insertdb(MYSQL *mysql, struct song_metadata *psong)
{
  if (psong->is_plist) {
    if (psong->plist_position) {
      return _insertdb_plist_item(mysql, psong);
    }
    else {
      return _insertdb_plist(mysql, psong);
    }
  }
  return _insertdb_song(mysql, psong);
}