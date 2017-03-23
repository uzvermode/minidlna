/* MiniDLNA media server
 * Copyright (C) 2008  Justin Maggard
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <libgen.h>
#include <errno.h>

#include "upnpglobalvars.h"
#include "libav.h"
#include "sql.h"
#include "utils.h"
#include "ffimg.h"
#include "log.h"
#include "albumart.h"

typedef struct
{
	image_size_enum type;
	const char* name;
	int width;
	int height;
} image_size_type_t;

static const image_size_type_t image_size_types[] = {
	{ JPEG_TN, "JPEG_TN", 160, 160 },
	{ JPEG_SM, "JPEG_SM", 640, 480 },
	{ JPEG_MED, "JPEG_MED", 1024, 768 },
	{ JPEG_LRG, "JPEG_LRG", 4096, 4096 },
	{ JPEG_INV, "", 0, 0 }
};

static const image_size_enum DEF_ALBUM_ART_BUILD_LEVEL = JPEG_LRG;

static inline album_art_t *_album_art_alloc()
{
	album_art_t *res;
	if (!(res = (album_art_t*)calloc(1,sizeof(album_art_t))))
	{
		DPRINTF(E_DEBUG, L_ARTWORK, "Unable to allocate album_art_t struct\n");
	}
	return res;
}

static inline int _copy_blob(album_art_t *album_art, const void *data, size_t data_size)
{
	if (!(album_art->image.blob.data = malloc(data_size)))
	{
		return 0;
	}
	memcpy(album_art->image.blob.data, data, data_size);
	album_art->image.blob.size = data_size;
	album_art->checksum = djb_hash(album_art->image.blob.data, album_art->image.blob.size);
	album_art->free_memory_block = 1;
	return 1;
}

static inline void _assign_blob(album_art_t *album_art, const void *data, size_t data_size)
{
	album_art->image.blob.data = (uint8_t*)data;
	album_art->image.blob.size = data_size;
	album_art->checksum = djb_hash(album_art->image.blob.data, album_art->image.blob.size);
	album_art->free_memory_block = 0;
}

image_size_enum album_art_get_profile(int width, int height)
{
	int i;
	int64_t pixels;

	inline int64_t number_of_pixels(int w, int h)
	{
		return (int64_t)w * h;
	}

	pixels = number_of_pixels(width, height);

	for(i=0; image_size_types[i].width; ++i)
	{
		if (number_of_pixels(image_size_types[i].width, image_size_types[i].height) > pixels) break;
	}

	return image_size_types[i].type;
}

static const image_size_type_t *_get_image_size_type(image_size_enum size_type)
{
	if (size_type < JPEG_TN || size_type > JPEG_LRG) size_type = JPEG_INV;
	return &image_size_types[size_type];
}

const char* album_art_get_size_name(image_size_enum size_type)
{
	const image_size_type_t *image_size = _get_image_size_type(size_type);
	return image_size->name;
}

void
album_art_update_cond(const char *path)
{
	char *dir;
	char *match;
	char file[MAXPATHLEN];
	char fpath[MAXPATHLEN];
	char dpath[MAXPATHLEN];
	int ncmp = 0;
	int album_art;
	DIR *dh;
	struct dirent *dp;
	int64_t art_id = 0;
	int ret;

	strncpyt(fpath, path, sizeof(fpath));
	match = basename(fpath);
	/* Check if this file name matches a specific audio or video file */
	if( ends_with(match, ".cover.jpg") )
	{
		ncmp = strlen(match)-10;
	}
	else
	{
		ncmp = strrchr(match, '.') - match;
	}
	/* Check if this file name matches one of the default album art names */
	album_art = is_album_art(match);

	strncpyt(dpath, path, sizeof(dpath));
	dir = dirname(dpath);
	dh = opendir(dir);
	if( !dh )
		return;
	while ((dp = readdir(dh)) != NULL)
	{
		snprintf(file, sizeof(file), "%s/%s", dir, dp->d_name);
		enum file_types type = resolve_file_type(dp, file, ALL_MEDIA);

		if( type != TYPE_FILE )
			continue;
		if( (dp->d_name[0] != '.') &&
		    (is_video(dp->d_name) || is_audio(dp->d_name)) &&
		    (album_art || strncmp(dp->d_name, match, ncmp) == 0) )
		{
			DPRINTF(E_DEBUG, L_METADATA, "New file %s looks like cover art for %s\n", path, dp->d_name);
			if ((art_id = album_art_add(file, NULL, 0, 0)))
			{
				ret = sql_exec(db, "UPDATE DETAILS set ALBUM_ART = %lld where PATH = '%q'", (long long)art_id, file);
				if( ret != SQLITE_OK )
					DPRINTF(E_DEBUG, L_METADATA, "Error setting %s as cover art for %s\n", match, dp->d_name);
			}
		}
	}
	closedir(dh);
}

static ffimg_t *_convert_unsupported_image(ffimg_t *img)
{
	ffimg_t *img_converted;

	if (ffimg_is_supported(img))
	{
		return img;
	}

	DPRINTF(E_DEBUG, L_METADATA, "Album art codec id is %d\n", (int)img->id);
	if ((img_converted = ffimg_resize(img, -1, -1, 1))) // without resizing, just load (and correct orientation)
        {
        	ffimg_free(img);
                return img_converted;
        }
        else
        {
		DPRINTF(E_WARN, L_ARTWORK, "Fail to convert album art\n");
                ffimg_free(img);
                return NULL;
        }

}

static int _convert_blob_to_jpeg(album_art_t *album_art, const uint8_t *image_data, size_t image_data_size, int make_copy)
{
	ffimg_t *img;

	if (!(img = ffimg_load_from_blob(image_data, image_data_size)))
	{
		DPRINTF(E_WARN, L_ARTWORK, "Could not load embedded album art\n");
		return 0;
	}
	else
	{
		ffimg_t *img_converted = _convert_unsupported_image(img);
		if (!img_converted)
		{
			return 0;
		}
		else if (img_converted == img)
		{
			ffimg_free(img);
			img = NULL;
		}
		else
		{
			img = img_converted;
		}
	}

	if (img)
	{
		int res;
		if (!(res = _copy_blob(album_art, img->packet->data, img->packet->size)))
		{
			DPRINTF(E_DEBUG, L_ARTWORK, "Cannot allocate memory block (converted) [%lld]\n", (long long)img->packet->size);
		}
		ffimg_free(img);
		return res;
	}
	else
	{
		int res = 1;
		if (!make_copy)
		{
			_assign_blob(album_art, image_data, image_data_size);
		}
		else
		{
			if (!(res = _copy_blob(album_art, image_data, image_data_size)))
			{
				DPRINTF(E_DEBUG, L_ARTWORK, "Cannot allocate memory block [%lld]\n", (long long)image_data_size);
			}
		}
		return res;
	}
}

static album_art_t *_create_from_img(const ffimg_t *img, int checksum, time_t timestamp)
{
	album_art_t *res;
	if (!(res = _album_art_alloc()))
	{
		return NULL;
	}

	if (!(res->image.blob.data = malloc(img->packet->size)))
	{
		free(res);
		return NULL;
	}
	
	memcpy(res->image.blob.data, img->packet->data, img->packet->size);
	res->image.blob.size = img->packet->size;
	res->is_blob = 1;
	res->free_memory_block = 1;
	res->checksum = checksum;
	res->timestamp = timestamp;
	return res;
}

static album_art_t *_convert_to_jpeg(album_art_t *album_art)
{
	ffimg_t *img;
	album_art_t *cnv_album_art;

	if (!(img = ffimg_load_from_file(album_art->image.path)))
	{
		DPRINTF(E_DEBUG, L_ARTWORK, "Cannot load album art from %s\n", album_art->image.path);
		album_art_free(album_art);
		return NULL;
	}
	else
	{
		ffimg_t *img_converted = _convert_unsupported_image(img);
		if (!img_converted)
		{
			album_art_free(album_art);
			return NULL;
		}
		else if (img == img_converted)
		{
			ffimg_free(img);
			return album_art;
		}
		else
		{
			img = img_converted;
		}
	}

	cnv_album_art = _create_from_img(img, album_art->checksum, album_art->timestamp);

	ffimg_free(img);
	album_art_free(album_art);
	return cnv_album_art;
}

static album_art_t *_create_album_art_from_blob(const uint8_t *image_data, size_t image_data_size, int make_copy, const char* path)
{
	struct stat st;
	album_art_t *res;

	if (lstat(path, &st))
	{
		return NULL;
	}

	if (!(res = _album_art_alloc()))
	{
		return NULL;
	}

	if (!_convert_blob_to_jpeg(res, image_data, image_data_size, make_copy))
	{
		free(res);
		return NULL;
	}

	res->is_blob = 1;
	res->timestamp = st.st_mtime;
	return res;
}


static inline int _check_file_specific_album_art_file(char *p, size_t s, const char *path, const char *suffix)
{
	snprintf(p, s, "%s.%s", path, suffix);
        return access(p, R_OK);
}

static album_art_t *_find_album_art(const char *path)
{
	char file[MAXPATHLEN];
	char mypath[MAXPATHLEN];
	struct linked_names_s *album_art_name;
	char *p;
	const char *dir;
	struct stat st;
	int ret;
	album_art_t *res = NULL;

	if (lstat(path, &st))
		return NULL;

	if( S_ISDIR(st.st_mode) )
	{
		dir = path;
		goto check_dir;
	}

	strncpyt(mypath, path, sizeof(mypath));
	dir = dirname(mypath);

	/* First look for file-specific cover art */
	for (album_art_name = album_art_names; album_art_name; album_art_name = album_art_name->next)
	{
		if (!(ret = _check_file_specific_album_art_file(file, sizeof(file), path, album_art_name->name)))
			goto add_cached_image;
	}

	// with changed file extension
	strncpyt(file, path, sizeof(file));
	if ((p = strrchr(file, '.')))
	{
		strcpy(p, ".jpg");
		if (!(ret = access(file, R_OK))) goto add_cached_image;

		strcpy(p, ".png");
		if (!(ret = access(file, R_OK))) goto add_cached_image;

		strcpy(p, ".webp");
		if (!(ret = access(file, R_OK))) goto add_cached_image;
	}

	// hidden file(s)
	strcpy(p, ".jpg");
	if ((p = strrchr(file, '/')))
	{
		char *p1;
		
		memmove(p+2, p+1, file+MAXPATHLEN-p-2);
		p[1] = '.';
		if (!(ret = access(file, R_OK))) goto add_cached_image;

		if (!(p1 = strrchr(file, '.'))) goto check_dir;
		
		strcpy(p1, ".png");
                if (!(ret = access(file, R_OK))) goto add_cached_image;

                strcpy(p1, ".webp");
                if (!(ret = access(file, R_OK))) goto add_cached_image;
	}

check_dir:
	/* Then fall back to possible generic cover art file names */
	for (album_art_name = album_art_names; album_art_name; album_art_name = album_art_name->next)
	{
		snprintf(file, sizeof(file), "%s/%s", dir, album_art_name->name);
		if ((ret = access(file, R_OK))) continue;

add_cached_image:
		DPRINTF(E_DEBUG, L_ARTWORK, "Found album art in %s\n", file);
		if (!(res = _album_art_alloc()))
		{
			return NULL;
		}

		res->is_blob = 0;
		res->free_memory_block = 1;
		res->image.path = strdup(file);

		if (lstat(file, &st))
		{
			DPRINTF(E_DEBUG, L_ARTWORK, "Could not acces %s", file);
			free(res->image.path);
			free(res);
			return NULL;
		}

		if (!djb_hash_from_file(file, &res->checksum))
		{
			DPRINTF(E_DEBUG, L_ARTWORK, "Could not calculate checksum of %s\n", file);
			free(res->image.path);
			free(res);
			return NULL;
		}

		res->timestamp = st.st_mtime;
		return res;
	}

	return res;
}

static int64_t _find_album_art_by_checksum(uint32_t checksum, time_t *timestamp)
{
	sqlite3_stmt *stmt;
	int res;
	int64_t album_art_id = 0;

	res = sqlite3_prepare_v2(db, "SELECT ID,TIMESTAMP FROM ALBUM_ART WHERE PARENT IS NULL AND CHECKSUM=?", -1, &stmt, NULL);
	if (res != SQLITE_OK)
	{
		DPRINTF(E_ERROR, L_ARTWORK, "_find_album_art_by_checksum - fail to prepare statement [%d] [%s]\n", res, sqlite3_errmsg(db));
		return 0;
	}

	res = sqlite3_bind_int64(stmt, 1, checksum);
	if (sqlite3_step(stmt) == SQLITE_ROW)
	{
		album_art_id = sqlite3_column_int64(stmt, 0);
		*timestamp = (time_t)sqlite3_column_int64(stmt, 1);
	}

	sqlite3_finalize(stmt);
	return album_art_id;
}

static void _update_album_art_timestamp(int64_t id, time_t timestamp)
{
	sqlite3_stmt *stmt;
	int res;

	res = sqlite3_prepare_v2(db, "UPDATE ALBUM_ART SET TIMESTAMP=? WHERE ID=?", -1, &stmt, NULL);
	if (res != SQLITE_OK)
	{
		DPRINTF(E_ERROR, L_ARTWORK, "_update_album_art_timestamp - fail to prepare statement [%d] [%s]\n", res, sqlite3_errmsg(db));
		return;
	}

	res = sqlite3_bind_int64(stmt, 1, timestamp);
	res = sqlite3_bind_int64(stmt, 2, id);

	if ((res = sqlite3_step(stmt)) != SQLITE_DONE)
	{ // error here
		DPRINTF(E_INFO, L_ARTWORK, "_updata_album_art_timestamp(%lld) - fail to execute statement [%d] [%s]\n", (long long)id, res, sqlite3_errmsg(db));
	}

	sqlite3_finalize(stmt);
}

static int64_t _insert_album_art(const album_art_t *album_art)
{
	sqlite3_stmt *stmt;
	int res;
	int return_last_row_id = 0;

	res = sqlite3_prepare_v2(db, "INSERT INTO ALBUM_ART(PATH,CHECKSUM,TIMESTAMP,PARENT,PROFILE) VALUES(?,?,?,NULL,NULL)", -1, &stmt, NULL);
	if (res != SQLITE_OK)
	{
		DPRINTF(E_ERROR, L_ARTWORK, "_insert_album_art - fail to prepare statement [%d] [%s]\n", res, sqlite3_errmsg(db));
		return 0;
	}

	if (album_art->is_blob)
	{
		res = sqlite3_bind_blob(stmt, 1, album_art->image.blob.data, album_art->image.blob.size, SQLITE_STATIC);
	}
	else
	{
		res = sqlite3_bind_text(stmt, 1, album_art->image.path, -1, SQLITE_STATIC);
	}
	res = sqlite3_bind_int64(stmt, 2, album_art->checksum);
	res = sqlite3_bind_int64(stmt, 3, album_art->timestamp);
	
	switch(res = sqlite3_step(stmt))
	{
		case SQLITE_DONE:
		return_last_row_id = 1;
		break;

		default:
		DPRINTF(E_WARN, L_ARTWORK, "_insert_album_art - fail to execute statement [%d] [%s]\n", res, sqlite3_errmsg(db));
		break;
	}

	sqlite3_finalize(stmt);

	if (return_last_row_id)
	{
		return sqlite3_last_insert_rowid(db);
	}
	else
	{
		return 0;
	}
}

static int64_t _insert_sized_album_art(const album_art_t *album_art, image_size_enum image_size, uint64_t parent_album_art_id)
{
	sqlite3_stmt *stmt;
	int res;
	int return_last_row_id = 0;
	int already_in_table = 0;

	res = sqlite3_prepare_v2(db, "INSERT INTO ALBUM_ART(PATH,CHECKSUM,TIMESTAMP,PARENT,PROFILE) VALUES (?,?,?,?,?)", -1, &stmt, NULL);
	if (res != SQLITE_OK)
	{
		DPRINTF(E_ERROR, L_ARTWORK, "_insert_sized_album_art - fail to prepare statement [%d] [%s]\n", res, sqlite3_errmsg(db));
		return 0;
	}

	if (album_art)
	{
		if (album_art->is_blob)
		{
			res = sqlite3_bind_blob(stmt, 1, album_art->image.blob.data, album_art->image.blob.size, SQLITE_STATIC);
		}
		else
		{
			res = sqlite3_bind_text(stmt, 1, album_art->image.path, -1, SQLITE_STATIC);
		}
		res = sqlite3_bind_int64(stmt, 2, album_art->checksum);
		res = sqlite3_bind_int64(stmt, 3, album_art->timestamp);
	}
	else
	{
		res = sqlite3_bind_int64(stmt, 1, parent_album_art_id);
		res = sqlite3_bind_null(stmt, 2); // parent's checksum
		res = sqlite3_bind_null(stmt, 3); // parent's timestamp
	}
	res = sqlite3_bind_int64(stmt, 4, parent_album_art_id);
	res = sqlite3_bind_int(stmt, 5, image_size);

	switch((res = sqlite3_step(stmt)))
	{
		case SQLITE_DONE:
		return_last_row_id = 1;
		break;

		case SQLITE_CONSTRAINT:
		DPRINTF(E_DEBUG, L_ARTWORK, "_insert_sized_album_art(%lld,%d) - [%d] [%s]\n", (long long)parent_album_art_id, (int)image_size, res, sqlite3_errmsg(db));
		already_in_table = 1;
		break;

		default:
		DPRINTF(E_WARN, L_ARTWORK, "_insert_sized_album_art(%lld,%d) - fail to execute statement [%d] [%s]\n", (long long)parent_album_art_id, (int)image_size, res, sqlite3_errmsg(db));
		break;
	}

	sqlite3_finalize(stmt);

	if (return_last_row_id)
	{
		return sqlite3_last_insert_rowid(db);
	}
	else if (already_in_table)
	{
		return -1;
	}
	else
	{
		return 0;
	}
}

static inline ffimg_t *_load_image_from_album_art(const album_art_t *album_art)
{
	if (album_art->is_blob)
	{
		return ffimg_load_from_blob(album_art->image.blob.data, album_art->image.blob.size);
	}
	else
	{
		return ffimg_load_from_file(album_art->image.path);
	}
}

static int64_t _create_sized_from_image(const ffimg_t* img, int64_t album_art_id, image_size_enum image_size, time_t timestamp)
{
	const image_size_type_t *image_size_type;
	int width, height;
	int leave_as_is = 0;
	ffimg_t *img_resized = NULL;

	if (!(image_size_type = _get_image_size_type(image_size)))
	{
		return 0;
	}

	ffimg_get_dimensions(img, &width, &height);

	if (image_size_type->width>width && image_size_type->height>height)
	{ // don't upsize
		leave_as_is = 1;
	}
	else
	{
		if (!(img_resized = ffimg_resize(img, image_size_type->width, image_size_type->height, 1)))
		{
			DPRINTF(E_WARN, L_ARTWORK, "_create_sized_from_image(%lld,%d) - fail to resize picture\n", (long long)album_art_id, (int)image_size);
			leave_as_is = 1;
		}
	}

	if (leave_as_is || img_resized)
	{
		int64_t res = 0;

		if (leave_as_is)
		{
			res = _insert_sized_album_art(NULL, image_size, album_art_id);
		}
		else
		{
			album_art_t *album_art;

			if ((album_art = _album_art_alloc()))
			{
				album_art->is_blob = 1;
				_assign_blob(album_art, img_resized->packet->data, img_resized->packet->size);
				album_art->timestamp = timestamp;
				res = _insert_sized_album_art(album_art, image_size, album_art_id);
				album_art_free(album_art);
			}
		}
		ffimg_free(img_resized);
		if (res)
		{
			DPRINTF(E_DEBUG, L_ARTWORK, "_create_sized_from_image(%lld,%d) - added new element [%lld,%d]\n", (long long)album_art_id, (int)image_size, (long long)res, leave_as_is);
		}
		return res;
	}

	return 0;
}

static void _create_sized(const album_art_t *album_art, int64_t album_art_id, image_size_enum build_level)
{
	ffimg_t *img;
	image_size_enum i;

	if (!(img = _load_image_from_album_art(album_art)))
	{
		return;
	}

	for(i=JPEG_TN; i<=build_level; ++i)
	{
		if (!_create_sized_from_image(img, album_art_id, i, album_art->timestamp))
		{
			DPRINTF(E_DEBUG, L_ARTWORK, "_create_sized(%lld,%d) - fail to create sized variant\n", (long long)album_art_id, (int)i);
		}
	}

	ffimg_free(img);
}

int64_t album_art_add(const char *path, const uint8_t *image_data, size_t image_data_size, int make_copy)
{
	album_art_t *album_art = NULL;
	time_t old_timestamp = 0;
	int new_album_art = 0;
	int64_t res = 0;

	if (image_data && image_data_size)
	{
		album_art = _create_album_art_from_blob(image_data, image_data_size, make_copy, path);
	}
	if (!album_art)
	{
		if ((album_art = _find_album_art(path)))
		{
			album_art = _convert_to_jpeg(album_art);
		}
	}

	// no album art
	if (!album_art) return 0;

	if ((res = _find_album_art_by_checksum(album_art->checksum, &old_timestamp)))
	{ // update record
		if (album_art->timestamp != old_timestamp)
		{
			_update_album_art_timestamp(res, album_art->timestamp);
		}
	}
	else
	{ // insert new record
		if ((new_album_art = (res = _insert_album_art(album_art)) != 0))
		{
			DPRINTF(E_DEBUG, L_ARTWORK, "album_art_add(%s) - added new element [%lld]\n", path, (long long)res);
		}
	}

	if (new_album_art)
	{
		_create_sized(album_art, res, DEF_ALBUM_ART_BUILD_LEVEL);
	}

	album_art_free(album_art);
	return res;
}

static int _is_album_art_valid(const album_art_t *album_art)
{
	if (album_art->is_blob)
		return (album_art->image.blob.data != NULL) && album_art->image.blob.size;
	else
		return (album_art->image.path != NULL);
}

album_art_t *album_art_get(int64_t album_art_id, image_size_enum image_size)
{
	sqlite3_stmt *stmt;
	int column_type, res;
	album_art_t *album_art = NULL;
	int return_parent = 0;

	if (image_size == JPEG_INV)
	{
		res = sqlite3_prepare_v2(db, "SELECT PATH,CHECKSUM,TIMESTAMP FROM ALBUM_ART WHERE ID=? AND PARENT IS NULL", -1, &stmt, NULL);
		if (res != SQLITE_OK)
		{
			DPRINTF(E_ERROR, L_ARTWORK, "album_art_get(1) - fail to prepare statement [%d] [%s]\n", res, sqlite3_errmsg(db));
			return NULL;
		}
		res = sqlite3_bind_int64(stmt, 1, album_art_id);
	}
	else
	{
		res = sqlite3_prepare_v2(db, "SELECT PATH,CHECKSUM,TIMESTAMP FROM ALBUM_ART WHERE PARENT=? AND PROFILE=?", -1, &stmt, NULL);
		if (res != SQLITE_OK)
		{
			DPRINTF(E_ERROR, L_ARTWORK, "album_art_get(2) - fail to prepare statement [%d] [%s]\n", res, sqlite3_errmsg(db));
			return NULL;
		}
		res = sqlite3_bind_int64(stmt, 1, album_art_id);
		res = sqlite3_bind_int(stmt, 2, image_size);
	}

	switch((res = sqlite3_step(stmt)))
	{
		case SQLITE_ROW:
		{
			switch( (column_type=sqlite3_column_type(stmt, 0)) )
			{
				case SQLITE_INTEGER:
				{
					return_parent = (image_size != JPEG_INV) && (sqlite3_column_int64(stmt, 0) == album_art_id);
					break;
				}

				case SQLITE_TEXT:
				{
					if ((album_art = _album_art_alloc()))
					{
						album_art->image.path = strdup((const char*)sqlite3_column_text(stmt, 0));
						album_art->is_blob = 0;
						album_art->free_memory_block = 1;
						album_art->checksum = (uint32_t)sqlite3_column_int64(stmt, 1);
						album_art->timestamp = sqlite3_column_int64(stmt, 2);
					}
					break;
				}

				case SQLITE_BLOB:
				{
					if ((album_art = _album_art_alloc()))
					{
						size_t nbytes = sqlite3_column_bytes(stmt, 0);
						if ((album_art->image.blob.data = malloc(nbytes)))
						{
							memcpy(album_art->image.blob.data, sqlite3_column_blob(stmt, 0), nbytes);
						}
						else
						{
							DPRINTF(E_DEBUG, L_ARTWORK, "album_art_get(%lld,%d) fail to allocate memory block %lld\n", (long long)album_art_id, (int)image_size, (long long)nbytes);
						}
						album_art->image.blob.size = nbytes;
						album_art->is_blob = 1;
						album_art->free_memory_block = 1;
						album_art->checksum = (uint32_t)sqlite3_column_int64(stmt, 1);
						album_art->timestamp = sqlite3_column_int64(stmt, 2);
					}
					break;
				}

				default:
				{ // report error, unsupported column type
					DPRINTF(E_ERROR, L_ARTWORK, "album_art_get(%lld,%d) - unexpected column type %d\n", (long long)album_art_id, (int)image_size, column_type);
				}
			}
			break;
		}

		case SQLITE_DONE:
		break;

		default:
		DPRINTF(E_WARN, L_ARTWORK, "album_art_get(%lld,%d) - fail to execute statement [%d] [%s]\n", (long long)album_art_id, (int)image_size, res, sqlite3_errmsg(db));
		break;
	}

	sqlite3_finalize(stmt);

	if (return_parent)
	{ // return parent
		return album_art_get(album_art_id, JPEG_INV);
	}

	if (album_art && !_is_album_art_valid(album_art))
	{
		album_art_free(album_art);
		album_art = NULL;
	}

	return album_art;
}

int64_t album_art_create_sized(int64_t album_art_id, image_size_enum image_size)
{
	album_art_t *album_art;
	ffimg_t *img;
	int64_t res;

	if (!(album_art = album_art_get(album_art_id, JPEG_INV)))
	{
		return 0;
	}

	if (!(img = _load_image_from_album_art(album_art)))
	{
		album_art_free(album_art);
		return 0;
	}

	res = _create_sized_from_image(img, album_art_id, image_size, album_art->timestamp);

	ffimg_free(img);
	album_art_free(album_art);

	return res;
}

int album_art_check(int64_t album_art_id)
{
	sqlite3_stmt *stmt;
	int res;
	int check = 0;

	res = sqlite3_prepare(db, "SELECT * FROM ALBUM_ART WHERE ID=? AND PARENT IS NULL", -1, &stmt, NULL);
	if (res != SQLITE_OK)
	{
		DPRINTF(E_ERROR, L_ARTWORK, "album_art_check fail to prepare statement [%d] [%s]\n", res, sqlite3_errmsg(db));
		return 0;
	}

	res = sqlite3_bind_int64(stmt, 1, album_art_id);
	res = sqlite3_step(stmt);
	check = (res == SQLITE_ROW);
	res = sqlite3_finalize(stmt);

	return check;
}

void album_art_free(album_art_t *album_art)
{
	if (!album_art) return;
	if (album_art->free_memory_block)
	{
		free(album_art->is_blob? (void*)album_art->image.blob.data : (void*)album_art->image.path);
	}
	free(album_art);
}
