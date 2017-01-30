/* MiniDLNA media server
 * Copyright (C) 2008-2009  Justin Maggard
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
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <fcntl.h>

#include <libexif/exif-loader.h>
#include <jpeglib.h>
#include <setjmp.h>
#include "libav.h"

#include "upnpglobalvars.h"
#include "tagutils/tagutils.h"
#include "image_utils.h"
#include "upnpreplyparse.h"
#include "tivo_utils.h"
#include "metadata.h"
#include "albumart.h"
#include "utils.h"
#include "sql.h"
#include "log.h"

//require libdvdread
#include <dvdread/ifo_read.h>

#define FLAG_TITLE	0x00000001
#define FLAG_ARTIST	0x00000002
#define FLAG_ALBUM	0x00000004
#define FLAG_GENRE	0x00000008
#define FLAG_COMMENT	0x00000010
#define FLAG_CREATOR	0x00000020
#define FLAG_DATE	0x00000040
#define FLAG_DLNA_PN	0x00000080
#define FLAG_MIME	0x00000100
#define FLAG_DURATION	0x00000200
#define FLAG_RESOLUTION	0x00000400

/* Audio profile flags */
enum audio_profiles {
	PROFILE_AUDIO_UNKNOWN,
	PROFILE_AUDIO_MP3,
	PROFILE_AUDIO_AC3,
	PROFILE_AUDIO_WMA_BASE,
	PROFILE_AUDIO_WMA_FULL,
	PROFILE_AUDIO_WMA_PRO,
	PROFILE_AUDIO_MP2,
	PROFILE_AUDIO_PCM,
	PROFILE_AUDIO_AAC,
	PROFILE_AUDIO_AAC_MULT5,
	PROFILE_AUDIO_AMR,
	PROFILE_AUDIO_DTS
};

/* This function shamelessly copied from libdlna */
#define MPEG_TS_SYNC_CODE 0x47
#define MPEG_TS_PACKET_LENGTH 188
#define MPEG_TS_PACKET_LENGTH_DLNA 192 /* prepends 4 bytes to TS packet */
int
dlna_timestamp_is_present(const char *filename, int *raw_packet_size)
{
	unsigned char buffer[3*MPEG_TS_PACKET_LENGTH_DLNA];
	int fd, i;

	/* read file header */
	fd = open(filename, O_RDONLY);
	if( fd < 0 )
		return 0;
	i = read(fd, buffer, MPEG_TS_PACKET_LENGTH_DLNA*3);
	close(fd);
	if( i < 0 )
		return 0;
	for( i = 0; i < MPEG_TS_PACKET_LENGTH_DLNA; i++ )
	{
		if( buffer[i] == MPEG_TS_SYNC_CODE )
		{
			if (buffer[i + MPEG_TS_PACKET_LENGTH_DLNA] == MPEG_TS_SYNC_CODE &&
			    buffer[i + MPEG_TS_PACKET_LENGTH_DLNA*2] == MPEG_TS_SYNC_CODE)
			{
			        *raw_packet_size = MPEG_TS_PACKET_LENGTH_DLNA;
				if (buffer[i+MPEG_TS_PACKET_LENGTH] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+1] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+2] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+3] == 0x00)
					return 0;
				else
					return 1;
			} else if (buffer[i + MPEG_TS_PACKET_LENGTH] == MPEG_TS_SYNC_CODE &&
				   buffer[i + MPEG_TS_PACKET_LENGTH*2] == MPEG_TS_SYNC_CODE) {
			    *raw_packet_size = MPEG_TS_PACKET_LENGTH;
			    return 0;
			}
		}
	}
	*raw_packet_size = 0;
	return 0;
}

void
check_for_captions(const char *path, int64_t detailID)
{
	char file[MAXPATHLEN];
	char *p;
	int ret;

	strncpyt(file, path, sizeof(file));
	p = strip_ext(file);
	if (!p)
		p = strrchr(file, '\0');

	/* If we weren't given a detail ID, look for one. */
	if (!detailID)
	{
		detailID = sql_get_int64_field(db, "SELECT ID from DETAILS where (PATH > '%q.' and PATH <= '%q.z')"
		                            " and MIME glob 'video/*' limit 1", file, file);
		if (detailID <= 0)
		{
			//DPRINTF(E_MAXDEBUG, L_METADATA, "No file found for caption %s.\n", path);
			return;
		}
	}

	strcpy(p, ".srt");
	ret = access(file, R_OK);
	if (ret != 0)
	{
		strcpy(p, ".smi");
		ret = access(file, R_OK);
	}

	if (ret == 0)
	{
		sql_exec(db, "INSERT OR REPLACE into CAPTIONS"
		             " (ID, PATH) "
		             "VALUES"
		             " (%lld, %Q)", detailID, file);
	}
}

void
parse_nfo(const char *path, metadata_t *m)
{
	FILE *nfo;
	char buf[65536];
	struct NameValueParserData xml;
	struct stat file;
	size_t nread;
	char *val, *val2;

	if( stat(path, &file) != 0 ||
	    file.st_size > 65536 )
	{
		DPRINTF(E_INFO, L_METADATA, "Not parsing very large .nfo file %s\n", path);
		return;
	}
	DPRINTF(E_DEBUG, L_METADATA, "Parsing .nfo file: %s\n", path);
	nfo = fopen(path, "r");
	if( !nfo )
		return;
	nread = fread(&buf, 1, sizeof(buf), nfo);
	
	ParseNameValue(buf, nread, &xml, 0);

	//printf("\ttype: %s\n", GetValueFromNameValueList(&xml, "rootElement"));
	val = GetValueFromNameValueList(&xml, "title");
	if( val )
	{
		char *esc_tag, *title;
		val2 = GetValueFromNameValueList(&xml, "episodetitle");
		if( val2 )
			xasprintf(&title, "%s - %s", val, val2);
		else
			title = strdup(val);
		esc_tag = unescape_tag(title, 1);
		m->title = escape_tag(esc_tag, 1);
		free(esc_tag);
		free(title);
	}

	val = GetValueFromNameValueList(&xml, "plot");
	if( val ) {
		char *esc_tag = unescape_tag(val, 1);
		m->comment = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "capturedate");
	if( val ) {
		char *esc_tag = unescape_tag(val, 1);
		m->date = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "genre");
	if( val )
	{
		free(m->genre);
		char *esc_tag = unescape_tag(val, 1);
		m->genre = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "mime");
	if( val )
	{
		free(m->mime);
		char *esc_tag = unescape_tag(val, 1);
		m->mime = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	ClearNameValueList(&xml);
	fclose(nfo);
}

void
free_metadata(metadata_t *m, uint32_t flags)
{
	if( flags & FLAG_TITLE )
		free(m->title);
	if( flags & FLAG_ARTIST )
		free(m->artist);
	if( flags & FLAG_ALBUM )
		free(m->album);
	if( flags & FLAG_GENRE )
		free(m->genre);
	if( flags & FLAG_CREATOR )
		free(m->creator);
	if( flags & FLAG_DATE )
		free(m->date);
	if( flags & FLAG_COMMENT )
		free(m->comment);
	if( flags & FLAG_DLNA_PN )
		free(m->dlna_pn);
	if( flags & FLAG_MIME )
		free(m->mime);
	if( flags & FLAG_DURATION )
		free(m->duration);
	if( flags & FLAG_RESOLUTION )
		free(m->resolution);
}

int64_t
GetFolderMetadata(const char *name, const char *path, const char *artist, const char *genre, int64_t album_art)
{
	int ret;

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (TITLE, PATH, CREATOR, ARTIST, GENRE, ALBUM_ART) "
	                   "VALUES"
	                   " ('%q', %Q, %Q, %Q, %Q, %lld);",
	                   name, path, artist, artist, genre, album_art);
	if( ret != SQLITE_OK )
		ret = 0;
	else
		ret = sqlite3_last_insert_rowid(db);

	return ret;
}

int64_t
GetAudioMetadata(const char *path, char *name)
{
	char type[4];
	static char lang[6] = { '\0' };
	struct stat file;
	int64_t ret;
	char *esc_tag;
	int i;
	int64_t album_art = 0;
	struct song_metadata song;
	metadata_t m;
	uint32_t free_flags = FLAG_MIME|FLAG_DURATION|FLAG_DLNA_PN|FLAG_DATE;
	memset(&m, '\0', sizeof(metadata_t));

	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);

	if( ends_with(path, ".mp3") )
	{
		strcpy(type, "mp3");
		m.mime = strdup("audio/mpeg");
	}
	else if( ends_with(path, ".m4a") || ends_with(path, ".mp4") ||
	         ends_with(path, ".aac") || ends_with(path, ".m4p") )
	{
		strcpy(type, "aac");
		m.mime = strdup("audio/mp4");
	}
	else if( ends_with(path, ".3gp") )
	{
		strcpy(type, "aac");
		m.mime = strdup("audio/3gpp");
	}
	else if( ends_with(path, ".wma") || ends_with(path, ".asf") )
	{
		strcpy(type, "asf");
		m.mime = strdup("audio/x-ms-wma");
	}
	else if( ends_with(path, ".flac") || ends_with(path, ".fla") || ends_with(path, ".flc") )
	{
		strcpy(type, "flc");
		m.mime = strdup("audio/x-flac");
	}
	else if( ends_with(path, ".wav") )
	{
		strcpy(type, "wav");
		m.mime = strdup("audio/x-wav");
	}
	else if( ends_with(path, ".ogg") || ends_with(path, ".oga") )
	{
		strcpy(type, "ogg");
		m.mime = strdup("audio/ogg");
	}
	else if( ends_with(path, ".pcm") )
	{
		strcpy(type, "pcm");
		m.mime = strdup("audio/L16");
	}
	else
	{
		DPRINTF(E_WARN, L_METADATA, "Unhandled file extension on %s\n", path);
		return 0;
	}

	if( !(*lang) )
	{
		if( !getenv("LANG") )
			strcpy(lang, "en_US");
		else
			strncpyt(lang, getenv("LANG"), sizeof(lang));
	}

	if( readtags((char *)path, &song, &file, lang, type) != 0 )
	{
		DPRINTF(E_WARN, L_METADATA, "Cannot extract tags from %s!\n", path);
        	freetags(&song);
		free_metadata(&m, free_flags);
		return 0;
	}

	if( song.dlna_pn )
		m.dlna_pn = strdup(song.dlna_pn);
	if( song.year )
		xasprintf(&m.date, "%04d-01-01", song.year);
	xasprintf(&m.duration, "%d:%02d:%02d.%03d",
	                      (song.song_length/3600000),
	                      (song.song_length/60000%60),
	                      (song.song_length/1000%60),
	                      (song.song_length%1000));
	if( song.title && *song.title )
	{
		m.title = trim(song.title);
		if( (esc_tag = escape_tag(m.title, 0)) )
		{
			free_flags |= FLAG_TITLE;
			m.title = esc_tag;
		}
	}
	else
	{
		m.title = name;
	}
	for( i = ROLE_START; i < N_ROLE; i++ )
	{
		if( song.contributor[i] && *song.contributor[i] )
		{
			m.creator = trim(song.contributor[i]);
			if( strlen(m.creator) > 48 )
			{
				m.creator = strdup("Various Artists");
				free_flags |= FLAG_CREATOR;
			}
			else if( (esc_tag = escape_tag(m.creator, 0)) )
			{
				m.creator = esc_tag;
				free_flags |= FLAG_CREATOR;
			}
			m.artist = m.creator;
			break;
		}
	}
	/* If there is a album artist or band associated with the album,
	   use it for virtual containers. */
	if( i < ROLE_ALBUMARTIST )
	{
		for( i = ROLE_ALBUMARTIST; i <= ROLE_BAND; i++ )
		{
			if( song.contributor[i] && *song.contributor[i] )
				break;
		}
	        if( i <= ROLE_BAND )
		{
			m.artist = trim(song.contributor[i]);
			if( strlen(m.artist) > 48 )
			{
				m.artist = strdup("Various Artists");
				free_flags |= FLAG_ARTIST;
			}
			else if( (esc_tag = escape_tag(m.artist, 0)) )
			{
				m.artist = esc_tag;
				free_flags |= FLAG_ARTIST;
			}
		}
	}
	if( song.album && *song.album )
	{
		m.album = trim(song.album);
		if( (esc_tag = escape_tag(m.album, 0)) )
		{
			free_flags |= FLAG_ALBUM;
			m.album = esc_tag;
		}
	}
	if( song.genre && *song.genre )
	{
		m.genre = trim(song.genre);
		if( (esc_tag = escape_tag(m.genre, 0)) )
		{
			free_flags |= FLAG_GENRE;
			m.genre = esc_tag;
		}
	}
	if( song.comment && *song.comment )
	{
		m.comment = trim(song.comment);
		if( (esc_tag = escape_tag(m.comment, 0)) )
		{
			free_flags |= FLAG_COMMENT;
			m.comment = esc_tag;
		}
	}

	album_art = find_album_art(path, song.image, song.image_size);

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, SIZE, TIMESTAMP, DURATION, CHANNELS, BITRATE, SAMPLERATE, DATE,"
	                   "  TITLE, CREATOR, ARTIST, ALBUM, GENRE, COMMENT, DISC, TRACK, DLNA_PN, MIME, ALBUM_ART) "
	                   "VALUES"
	                   " (%Q, %lld, %lld, '%s', %d, %d, %d, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %d, %d, %Q, '%s', %lld);",
	                   path, (long long)file.st_size, (long long)file.st_mtime, m.duration, song.channels, song.bitrate,
	                   song.samplerate, m.date, m.title, m.creator, m.artist, m.album, m.genre, m.comment, song.disc,
	                   song.track, m.dlna_pn, song.mime?song.mime:m.mime, album_art);
	if( ret != SQLITE_OK )
	{
		DPRINTF(E_ERROR, L_METADATA, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
	}
        freetags(&song);
	free_metadata(&m, free_flags);

	return ret;
}

/* For libjpeg error handling */
jmp_buf setjmp_buffer;
static void
libjpeg_error_handler(j_common_ptr cinfo)
{
	cinfo->err->output_message (cinfo);
	longjmp(setjmp_buffer, 1);
	return;
}

int64_t
GetImageMetadata(const char *path, char *name)
{
	ExifData *ed;
	ExifEntry *e = NULL;
	ExifLoader *l;
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	FILE *infile;
	int width=0, height=0, thumb=0;
	char make[32], model[64] = {'\0'};
	char b[1024];
	struct stat file;
	int64_t ret;
	image_s *imsrc;
	metadata_t m;
	uint32_t free_flags = 0xFFFFFFFF;
	memset(&m, '\0', sizeof(metadata_t));

	//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Parsing %s...\n", path);
	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * size: %jd\n", file.st_size);

	/* MIME hard-coded to JPEG for now, until we add PNG support */
	m.mime = strdup("image/jpeg");

	l = exif_loader_new();
	exif_loader_write_file(l, path);
	ed = exif_loader_get_data(l);
	exif_loader_unref(l);
	if( !ed )
		goto no_exifdata;

	e = exif_content_get_entry (ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL);
	if( e || (e = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_DIGITIZED)) )
	{
		m.date = strdup(exif_entry_get_value(e, b, sizeof(b)));
		if( strlen(m.date) > 10 )
		{
			m.date[4] = '-';
			m.date[7] = '-';
			m.date[10] = 'T';
		}
		else {
			free(m.date);
			m.date = NULL;
		}
	}
	else {
		/* One last effort to get the date from XMP */
		image_get_jpeg_date_xmp(path, &m.date);
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * date: %s\n", m.date);

	e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MAKE);
	if( e )
	{
		strncpyt(make, exif_entry_get_value(e, b, sizeof(b)), sizeof(make));
		e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MODEL);
		if( e )
		{
			strncpyt(model, exif_entry_get_value(e, b, sizeof(b)), sizeof(model));
			if( !strcasestr(model, make) )
				snprintf(model, sizeof(model), "%s %s", make, exif_entry_get_value(e, b, sizeof(b)));
			m.creator = escape_tag(trim(model), 1);
		}
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * model: %s\n", model);

	e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
	if( e )
	{
		switch( exif_get_short(e->data, exif_data_get_byte_order(ed)) )
		{
		case 3:
			m.rotation = 180;
			break;
		case 6:
			m.rotation = 90;
			break;
		case 8:
			m.rotation = 270;
			break;
		default:
			m.rotation = 0;
			break;
		}
	}

	if( ed->size )
	{
		/* We might need to verify that the thumbnail is 160x160 or smaller */
		if( ed->size > 12000 )
		{
			imsrc = image_new_from_jpeg(NULL, 0, ed->data, ed->size, 1, ROTATE_NONE);
			if( imsrc )
			{
 				if( (imsrc->width <= 160) && (imsrc->height <= 160) )
					thumb = 1;
				image_free(imsrc);
			}
		}
		else
			thumb = 1;
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * thumbnail: %d\n", thumb);

	exif_data_unref(ed);

no_exifdata:
	/* If SOF parsing fails, then fall through to reading the JPEG data with libjpeg to get the resolution */
	if( image_get_jpeg_resolution(path, &width, &height) != 0 || !width || !height )
	{
		infile = fopen(path, "r");
		if( infile )
		{
			cinfo.err = jpeg_std_error(&jerr);
			jerr.error_exit = libjpeg_error_handler;
			jpeg_create_decompress(&cinfo);
			if( setjmp(setjmp_buffer) )
				goto error;
			jpeg_stdio_src(&cinfo, infile);
			jpeg_read_header(&cinfo, TRUE);
			jpeg_start_decompress(&cinfo);
			width = cinfo.output_width;
			height = cinfo.output_height;
			error:
			jpeg_destroy_decompress(&cinfo);
			fclose(infile);
		}
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * resolution: %dx%d\n", width, height);

	if( !width || !height )
	{
		free_metadata(&m, free_flags);
		return 0;
	}
	if( width <= 640 && height <= 480 )
		m.dlna_pn = strdup("JPEG_SM");
	else if( width <= 1024 && height <= 768 )
		m.dlna_pn = strdup("JPEG_MED");
	else if( (width <= 4096 && height <= 4096) || !GETFLAG(DLNA_STRICT_MASK) )
		m.dlna_pn = strdup("JPEG_LRG");
	xasprintf(&m.resolution, "%dx%d", width, height);

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, TITLE, SIZE, TIMESTAMP, DATE, RESOLUTION,"
	                    " ROTATION, THUMBNAIL, CREATOR, DLNA_PN, MIME) "
	                   "VALUES"
	                   " (%Q, '%q', %lld, %lld, %Q, %Q, %u, %d, %Q, %Q, %Q);",
	                   path, name, (long long)file.st_size, (long long)file.st_mtime, m.date,
	                   m.resolution, m.rotation, thumb, m.creator, m.dlna_pn, m.mime);
	if( ret != SQLITE_OK )
	{
		DPRINTF(E_ERROR, L_METADATA, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
	}
	free_metadata(&m, free_flags);

	return ret;
}

#define META_LINES 16
#define META_LINE_SIZE 512
char meta[META_LINES][META_LINE_SIZE];
char meta_path[PATH_MAX];
char path_dup[PATH_MAX];
char path_dup1[PATH_MAX];
char buf[META_LINE_SIZE];

void
checkNull(char* buf)
{
	if (buf[strlen(buf)-1] == '\n')
	{
	  buf[strlen(buf)-1] = '\0';
	}
}

int64_t
GetVideoMetadataLite(const char *path, char *name)
{
	struct stat file;
	int ret;

	sprintf(path_dup, "%s", path);
	sprintf(path_dup1, "%s", path);
	char *dir_end = memrchr(path_dup, '/', strlen(path_dup));
	*dir_end = '\0';
	DPRINTF(E_DEBUG, L_METADATA, "Dirpath...[%s]\n", path_dup);
	sprintf(meta_path, "%s/.meta/%s", path_dup, basename(path_dup1));

	if ( !GETFLAG(CACHE_METADATA_MASK) || stat(meta_path, &file) != 0 )
	{
		DPRINTF(E_DEBUG, L_METADATA, "Get actual meta: %s!\n", meta_path);
		return GetVideoMetadata(path, name);
	}
	else
	{
		DPRINTF(E_DEBUG, L_METADATA, "Reading metadata file...[%s]\n", meta_path);

		// Reading metadata file
		FILE *metadata_ex_d = fopen(meta_path, "r");
		if (metadata_ex_d)
		{
			int i;
			for (i = 0; i < META_LINES; i++)
			{
				fgets(meta[i], META_LINE_SIZE, metadata_ex_d);
				checkNull(meta[i]);
			}

			fclose(metadata_ex_d);

			long long int pi0 = strtoll(meta[0], NULL, 10);
			long int pi1 = strtol(meta[1], NULL, 10);
			long long int pi15 = strtoll(meta[15], NULL, 10);

			ret = sql_exec(db, "INSERT into DETAILS"
		                   " (PATH, SIZE, TIMESTAMP, DURATION, DATE, CHANNELS, BITRATE, SAMPLERATE, RESOLUTION,"
		                   "  TITLE, CREATOR, ARTIST, GENRE, COMMENT, DLNA_PN, MIME, ALBUM_ART) "
		                   "VALUES"
		                   " (%Q, %lld, %ld, %Q, %Q, %Q, %Q, %Q, %Q, '%q', %Q, %Q, %Q, %Q, %Q, '%q', %lld);",
		                   path, pi0, pi1, meta[2], meta[3], meta[4], meta[5], meta[6], meta[7], meta[8],
				   meta[9], meta[10], meta[11], meta[12], meta[13], meta[14], pi15);

			if( ret == SQLITE_OK )
			{
				ret = sqlite3_last_insert_rowid(db);
				check_for_captions(path, ret);
			}
			return ret;
		}
		else
		{
			return GetVideoMetadata(path, name);
		}
	}

}


static void
write_metadata_file(const char *path, struct stat* file, metadata_t *m, int64_t album_art)
{
	// Create metadata file
	struct stat file_stat;
		
	sprintf(path_dup, "%s", path);
	char *dir_end = memrchr(path_dup, '/', strlen(path_dup));
	*dir_end = '\0';
	DPRINTF(E_DEBUG, L_METADATA, "Dirpath...[%s]\n", path_dup);

	sprintf(meta_path, "%s/.meta/", path_dup);
	if ( stat(meta_path, &file_stat) != 0 )
	{
		if ( mkdir(meta_path, S_IRWXU|S_IRWXG) == 0 )
		{
			DPRINTF(E_DEBUG, L_METADATA, "Create metadata path...[%s]\n", meta_path);
		}
	}
	if ( stat(meta_path, &file_stat) != 0 )
	{
		DPRINTF(E_WARN, L_METADATA, "Unable to create metadata path...[%s]\n", meta_path);
	}
	else
	{
		sprintf(path_dup1, "%s", path);
		sprintf(meta_path, "%s/.meta/%s", path_dup, basename(path_dup1));

		//umask(S_IREAD|S_IWRITE|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
		FILE *metadata_ex_d = fopen(meta_path, "w");
		if (metadata_ex_d)
		{
			sprintf(buf, "%lld\n%ld\n%s\n%s\n%u\n%u\n%u\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%lld\n", (long long)file->st_size,
				file->st_mtime, m->duration, m->date, m->channels, m->bitrate, m->frequency, m->resolution, m->title,
				m->creator, m->artist, m->genre, m->comment, m->dlna_pn,
				m->mime, (long long)album_art);

			size_t towrite = strlen(buf);
			size_t n = fwrite(&buf, 1, towrite, metadata_ex_d);

			fclose(metadata_ex_d);

			if ( n != towrite )
			{
				DPRINTF(E_WARN, L_METADATA, "Unable to fully write metadata file...[%s]\n", meta_path);
				unlink(meta_path);
			}
		}
		else
		{
			DPRINTF(E_WARN, L_METADATA, "Unable to create metadata file...[%s]\n", meta_path);
		}
	}
}

int64_t
GetVideoMetadata(const char *path, char *name)
{
	struct stat file;
	int ret, i;
	struct tm *modtime;
	AVFormatContext *ctx = NULL;
	AVCodecContext *ac = NULL, *vc = NULL;
	int audio_stream = -1, video_stream = -1;
	enum audio_profiles audio_profile = PROFILE_AUDIO_UNKNOWN;
	char fourcc[4];
	int64_t album_art = 0;
	char nfo[MAXPATHLEN], *ext;
	struct song_metadata video;
	metadata_t m;
	uint32_t free_flags = 0xFFFFFFFF;
	char *path_cpy, *basepath;
	int audio_streams = 0;

	memset(&m, '\0', sizeof(m));
	memset(&video, '\0', sizeof(video));

	DPRINTF(E_DEBUG, L_METADATA, "Parsing video %s...\n", path);
	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * size: %jd\n", file.st_size);

	ret = lav_open(&ctx, path);
	if( ret != 0 )
	{
		char err[128];
		av_strerror(ret, err, sizeof(err));
		DPRINTF(E_WARN, L_METADATA, "Opening %s failed! [%s]\n", path, err);
		return 0;
	}
	//dump_format(ctx, 0, NULL, 0);
	for( i=0; i<ctx->nb_streams; i++)
	{
		if (ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			audio_streams++;
		}
		if( ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
		    audio_stream == -1 )
		{
			audio_stream = i;
			ac = ctx->streams[audio_stream]->codec;
			continue;
		}
		else if( ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
		         !lav_is_thumbnail_stream(ctx->streams[i], &m.thumb_data, &m.thumb_size) &&
		         video_stream == -1 )
		{
			video_stream = i;
			vc = ctx->streams[video_stream]->codec;
			continue;
		}
	}
	path_cpy = strdup(path);
	basepath = basename(path_cpy);
	if( !vc )
	{
		/* This must not be a video file. */
		lav_close(ctx);
		if( !is_audio(path) )
			DPRINTF(E_DEBUG, L_METADATA, "File %s does not contain a video stream.\n", basepath);
		free(path_cpy);
		return 0;
	}

	if( ac )
	{
		DPRINTF(E_DEBUG, L_METADATA, "Audio Streams: %i\n", audio_streams);

		aac_object_type_t aac_type = AAC_INVALID;
		switch( ac->codec_id )
		{
			case AV_CODEC_ID_MP3:
				audio_profile = PROFILE_AUDIO_MP3;
				break;
			case AV_CODEC_ID_AAC:
				if( !ac->extradata_size ||
				    !ac->extradata )
				{
					DPRINTF(E_DEBUG, L_METADATA, "No AAC type\n");
				}
				else
				{
					uint8_t data;
					memcpy(&data, ac->extradata, 1);
					aac_type = data >> 3;
				}
				switch( aac_type )
				{
					/* AAC Low Complexity variants */
					case AAC_LC:
					case AAC_LC_ER:
						if( ac->sample_rate < 8000 ||
						    ac->sample_rate > 48000 )
						{
							DPRINTF(E_DEBUG, L_METADATA, "Unsupported AAC: sample rate is not 8000 < %d < 48000\n",
								ac->sample_rate);
							break;
						}
						/* AAC @ Level 1/2 */
						if( ac->channels <= 2 &&
						    ac->bit_rate <= 576000 )
							audio_profile = PROFILE_AUDIO_AAC;
						else if( ac->channels <= 6 &&
							 ac->bit_rate <= 1440000 )
							audio_profile = PROFILE_AUDIO_AAC_MULT5;
						else
							DPRINTF(E_DEBUG, L_METADATA, "Unhandled AAC: %d channels, %d bitrate\n",
								ac->channels,
								ac->bit_rate);
						break;
					default:
						DPRINTF(E_DEBUG, L_METADATA, "Unhandled AAC type [%d]\n", aac_type);
						break;
				}
				break;
			case AV_CODEC_ID_AC3:
				audio_profile = PROFILE_AUDIO_AC3;
				break;
			case AV_CODEC_ID_DTS:
				audio_profile = PROFILE_AUDIO_DTS;
				break;
			case AV_CODEC_ID_WMAV1:
			case AV_CODEC_ID_WMAV2:
				/* WMA Baseline: stereo, up to 48 KHz, up to 192,999 bps */
				if ( ac->bit_rate <= 193000 )
					audio_profile = PROFILE_AUDIO_WMA_BASE;
				/* WMA Full: stereo, up to 48 KHz, up to 385 Kbps */
				else if ( ac->bit_rate <= 385000 )
					audio_profile = PROFILE_AUDIO_WMA_FULL;
				break;
			case AV_CODEC_ID_WMAPRO:
				audio_profile = PROFILE_AUDIO_WMA_PRO;
				break;
			case AV_CODEC_ID_MP2:
				audio_profile = PROFILE_AUDIO_MP2;
				break;
			case AV_CODEC_ID_AMR_NB:
				audio_profile = PROFILE_AUDIO_AMR;
				break;
			default:
				if( (ac->codec_id >= AV_CODEC_ID_PCM_S16LE) &&
				    (ac->codec_id < AV_CODEC_ID_ADPCM_IMA_QT) )
					audio_profile = PROFILE_AUDIO_PCM;
				else
					DPRINTF(E_DEBUG, L_METADATA, "Unhandled audio codec [0x%X]\n", ac->codec_id);
				break;
		}
		m.frequency = ac->sample_rate;
		m.channels = ac->channels;
	}
	if( vc )
	{
		int off;
		int duration, hours, min, sec, ms;
		ts_timestamp_t ts_timestamp = NONE;
		DPRINTF(E_DEBUG, L_METADATA, "Container: '%s' [%s]\n", ctx->iformat->name, basepath);
		xasprintf(&m.resolution, "%dx%d", vc->width, vc->height);
		if( ctx->bit_rate > 8 )
			m.bitrate = ctx->bit_rate / 8;
		if( ctx->duration > 0 ) {
			duration = (int)(ctx->duration / AV_TIME_BASE);
			hours = (int)(duration / 3600);
			min = (int)(duration / 60 % 60);
			sec = (int)(duration % 60);
			ms = (int)(ctx->duration / (AV_TIME_BASE/1000) % 1000);
			xasprintf(&m.duration, "%d:%02d:%02d.%03d", hours, min, sec, ms);
		}

		/* NOTE: The DLNA spec only provides for ASF (WMV), TS, PS, and MP4 containers.
		 * Skip DLNA parsing for everything else. */
		if( strcmp(ctx->iformat->name, "avi") == 0 )
		{
			xasprintf(&m.mime, "video/x-msvideo");
			if( vc->codec_id == AV_CODEC_ID_MPEG4 )
			{
        			fourcc[0] = vc->codec_tag     & 0xff;
			        fourcc[1] = vc->codec_tag>>8  & 0xff;
		        	fourcc[2] = vc->codec_tag>>16 & 0xff;
			        fourcc[3] = vc->codec_tag>>24 & 0xff;
				if( memcmp(fourcc, "XVID", 4) == 0 ||
				    memcmp(fourcc, "DX50", 4) == 0 ||
				    memcmp(fourcc, "DIVX", 4) == 0 )
					xasprintf(&m.creator, "DiVX");
			}
		}
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 &&
		         ends_with(path, ".mov") )
			xasprintf(&m.mime, "video/quicktime");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			xasprintf(&m.mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			xasprintf(&m.mime, "video/x-flv");
		if( m.mime )
			goto video_no_dlna;

		switch( vc->codec_id )
		{
			case AV_CODEC_ID_MPEG1VIDEO:
				if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					if( (vc->width  == 352) &&
					    (vc->height <= 288) )
					{
						m.dlna_pn = strdup("MPEG1");
					}
					xasprintf(&m.mime, "video/mpeg");
				}
				break;
			case AV_CODEC_ID_MPEG2VIDEO:
				m.dlna_pn = malloc(64);
				off = sprintf(m.dlna_pn, "MPEG_");
				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s MPEG2 TS packet size %d\n",
						video_stream, basepath, m.resolution, raw_packet_size);
					off += sprintf(m.dlna_pn+off, "TS_");
					if( (vc->width  >= 1280) &&
					    (vc->height >= 720) )
					{
						off += sprintf(m.dlna_pn+off, "HD_NA");
					}
					else
					{
						off += sprintf(m.dlna_pn+off, "SD_");
						if( (vc->height == 576) ||
						    (vc->height == 288) )
							off += sprintf(m.dlna_pn+off, "EU");
						else
							off += sprintf(m.dlna_pn+off, "NA");
					}
					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if (dlna_ts_present)
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					else if( raw_packet_size != MPEG_TS_PACKET_LENGTH )
					{
						DPRINTF(E_DEBUG, L_METADATA, "Unsupported DLNA TS packet size [%d] (%s)\n",
							raw_packet_size, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
					}
					switch( ts_timestamp )
					{
						case NONE:
							xasprintf(&m.mime, "video/mpeg");
							if( m.dlna_pn )
								off += sprintf(m.dlna_pn+off, "_ISO");
							break;
						case VALID:
							off += sprintf(m.dlna_pn+off, "_T");
						case EMPTY:
							xasprintf(&m.mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s MPEG2 PS\n",
						video_stream, basepath, m.resolution);
					off += sprintf(m.dlna_pn+off, "PS_");
					if( (vc->height == 576) ||
					    (vc->height == 288) )
						off += sprintf(m.dlna_pn+off, "PAL");
					else
						off += sprintf(m.dlna_pn+off, "NTSC");
					xasprintf(&m.mime, "video/mpeg");
				}
				else
				{
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s [%s] is %s non-DLNA MPEG2\n",
						video_stream, basepath, ctx->iformat->name, m.resolution);
					free(m.dlna_pn);
					m.dlna_pn = NULL;
				}
				break;
			case AV_CODEC_ID_H264:
				m.dlna_pn = malloc(128);
				off = sprintf(m.dlna_pn, "AVC_");

				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					AVRational display_aspect_ratio;
					int fps, interlaced;
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);

					off += sprintf(m.dlna_pn+off, "TS_");
					if (vc->sample_aspect_ratio.num) {
						av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
						          vc->width  * vc->sample_aspect_ratio.num,
						          vc->height * vc->sample_aspect_ratio.den,
						          1024*1024);
					}
					fps = lav_get_fps(ctx->streams[video_stream]);
					interlaced = lav_get_interlaced(vc, ctx->streams[video_stream]);
					if( ((((vc->width == 1920 || vc->width == 1440) && vc->height == 1080) ||
					      (vc->width == 720 && vc->height == 480)) && fps == 59 && interlaced) ||
					    ((vc->width == 1280 && vc->height == 720) && fps == 59 && !interlaced) )
					{
						if( (vc->profile == FF_PROFILE_H264_MAIN || vc->profile == FF_PROFILE_H264_HIGH) &&
						    audio_profile == PROFILE_AUDIO_AC3 )
						{
							off += sprintf(m.dlna_pn+off, "HD_60_");
							vc->profile = FF_PROFILE_SKIP;
						}
					}
					else if( ((vc->width == 1920 && vc->height == 1080) ||
					          (vc->width == 1440 && vc->height == 1080) ||
					          (vc->width == 1280 && vc->height ==  720) ||
					          (vc->width ==  720 && vc->height ==  576)) &&
					          interlaced && fps == 50 )
					{
						if( (vc->profile == FF_PROFILE_H264_MAIN || vc->profile == FF_PROFILE_H264_HIGH) &&
						    audio_profile == PROFILE_AUDIO_AC3 )
						{
							off += sprintf(m.dlna_pn+off, "HD_50_");
							vc->profile = FF_PROFILE_SKIP;
						}
					}
					switch( vc->profile )
					{
						case FF_PROFILE_H264_BASELINE:
						case FF_PROFILE_H264_CONSTRAINED_BASELINE:
							off += sprintf(m.dlna_pn+off, "BL_");
							if( vc->width  <= 352 &&
							    vc->height <= 288 &&
							    vc->bit_rate <= 384000 )
							{
								off += sprintf(m.dlna_pn+off, "CIF15_");
								break;
							}
							else if( vc->width  <= 352 &&
							         vc->height <= 288 &&
							         vc->bit_rate <= 3000000 )
							{
								off += sprintf(m.dlna_pn+off, "CIF30_");
								break;
							}
							/* Fall back to Main Profile if it doesn't match a Baseline DLNA profile. */
							else
								off -= 3;
						default:
						case FF_PROFILE_H264_MAIN:
							off += sprintf(m.dlna_pn+off, "MP_");
							if( vc->profile != FF_PROFILE_H264_BASELINE &&
							    vc->profile != FF_PROFILE_H264_CONSTRAINED_BASELINE &&
							    vc->profile != FF_PROFILE_H264_MAIN )
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unknown AVC profile %d; assuming MP. [%s]\n",
									vc->profile, basepath);
							}
							if( vc->width  <= 720 &&
							    vc->height <= 576 &&
							    vc->bit_rate <= 10000000 )
							{
								off += sprintf(m.dlna_pn+off, "SD_");
							}
							else if( vc->width  <= 1920 &&
							         vc->height <= 1152 &&
							         vc->bit_rate <= 20000000 )
							{
								off += sprintf(m.dlna_pn+off, "HD_");
							}
							else
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 video profile! [%s, %dx%d, %dbps : %s]\n",
									m.dlna_pn, vc->width, vc->height, vc->bit_rate, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
							}
							break;
						case FF_PROFILE_H264_HIGH:
							off += sprintf(m.dlna_pn+off, "HP_");
							if( vc->width  <= 1920 &&
							    vc->height <= 1152 &&
							    vc->bit_rate <= 30000000 &&
							    audio_profile == PROFILE_AUDIO_AC3 )
							{
								off += sprintf(m.dlna_pn+off, "HD_");
							}
							else
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 HP video profile! [%dbps, %d audio : %s]\n",
									vc->bit_rate, audio_profile, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
							}
							break;
						case FF_PROFILE_SKIP:
							break;
					}
					if( !m.dlna_pn )
						break;
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MPEG1_L3");
							break;
						case PROFILE_AUDIO_AC3:
							off += sprintf(m.dlna_pn+off, "AC3");
							break;
						case PROFILE_AUDIO_AAC:
						case PROFILE_AUDIO_AAC_MULT5:
							off += sprintf(m.dlna_pn+off, "AAC_MULT5");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for %s file [%s]\n",
								m.dlna_pn, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
					if( !m.dlna_pn )
						break;
					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if( vc->profile == FF_PROFILE_H264_HIGH ||
						    dlna_ts_present )
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					else if( raw_packet_size != MPEG_TS_PACKET_LENGTH )
					{
						DPRINTF(E_DEBUG, L_METADATA, "Unsupported DLNA TS packet size [%d] (%s)\n",
							raw_packet_size, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
					}
					switch( ts_timestamp )
					{
						case NONE:
							if( m.dlna_pn )
								off += sprintf(m.dlna_pn+off, "_ISO");
							break;
						case VALID:
							off += sprintf(m.dlna_pn+off, "_T");
						case EMPTY:
							xasprintf(&m.mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					off += sprintf(m.dlna_pn+off, "MP4_");

					switch( vc->profile ) {
					case FF_PROFILE_H264_BASELINE:
					case FF_PROFILE_H264_CONSTRAINED_BASELINE:
						if( vc->width  <= 352 &&
						    vc->height <= 288 )
						{
							if( ctx->bit_rate < 600000 )
								off += sprintf(m.dlna_pn+off, "BL_CIF15_");
							else if( ctx->bit_rate < 5000000 )
								off += sprintf(m.dlna_pn+off, "BL_CIF30_");
							else
								goto mp4_mp_fallback;

							if( audio_profile == PROFILE_AUDIO_AMR )
							{
								off += sprintf(m.dlna_pn+off, "AMR");
							}
							else if( audio_profile == PROFILE_AUDIO_AAC )
							{
								off += sprintf(m.dlna_pn+off, "AAC_");
								if( ctx->bit_rate < 520000 )
								{
									off += sprintf(m.dlna_pn+off, "520");
								}
								else if( ctx->bit_rate < 940000 )
								{
									off += sprintf(m.dlna_pn+off, "940");
								}
								else
								{
									off -= 13;
									goto mp4_mp_fallback;
								}
							}
							else
							{
								off -= 9;
								goto mp4_mp_fallback;
							}
						}
						else if( vc->width  <= 720 &&
						         vc->height <= 576 )
						{
							if( vc->level == 30 &&
							    audio_profile == PROFILE_AUDIO_AAC &&
							    ctx->bit_rate <= 5000000 )
								off += sprintf(m.dlna_pn+off, "BL_L3L_SD_AAC");
							else if( vc->level <= 31 &&
							         audio_profile == PROFILE_AUDIO_AAC &&
							         ctx->bit_rate <= 15000000 )
								off += sprintf(m.dlna_pn+off, "BL_L31_HD_AAC");
							else
								goto mp4_mp_fallback;
						}
						else if( vc->width  <= 1280 &&
						         vc->height <= 720 )
						{
							if( vc->level <= 31 &&
							    audio_profile == PROFILE_AUDIO_AAC &&
							    ctx->bit_rate <= 15000000 )
								off += sprintf(m.dlna_pn+off, "BL_L31_HD_AAC");
							else if( vc->level <= 32 &&
							         audio_profile == PROFILE_AUDIO_AAC &&
							         ctx->bit_rate <= 21000000 )
								off += sprintf(m.dlna_pn+off, "BL_L32_HD_AAC");
							else
								goto mp4_mp_fallback;
						}
						else
							goto mp4_mp_fallback;
						break;
					case FF_PROFILE_H264_MAIN:
					mp4_mp_fallback:
						off += sprintf(m.dlna_pn+off, "MP_");
						/* AVC MP4 SD profiles - 10 Mbps max */
						if( vc->width  <= 720 &&
						    vc->height <= 576 &&
						    vc->bit_rate <= 10000000 )
						{
							sprintf(m.dlna_pn+off, "SD_");
							if( audio_profile == PROFILE_AUDIO_AC3 )
								off += sprintf(m.dlna_pn+off, "AC3");
							else if( audio_profile == PROFILE_AUDIO_AAC ||
							         audio_profile == PROFILE_AUDIO_AAC_MULT5 )
								off += sprintf(m.dlna_pn+off, "AAC_MULT5");
							else if( audio_profile == PROFILE_AUDIO_MP3 )
								off += sprintf(m.dlna_pn+off, "MPEG1_L3");
							else
								m.dlna_pn[10] = '\0';
						}
						else if( vc->width  <= 1280 &&
						         vc->height <= 720 &&
						         vc->bit_rate <= 15000000 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HD_720p_AAC");
						}
						else if( vc->width  <= 1920 &&
						         vc->height <= 1080 &&
						         vc->bit_rate <= 21000000 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HD_1080i_AAC");
						}
						if( strlen(m.dlna_pn) <= 11 )
						{
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for %s file %s\n",
								m.dlna_pn, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
						}
						break;
					case FF_PROFILE_H264_HIGH:
						if( vc->width  <= 1920 &&
						    vc->height <= 1080 &&
						    vc->bit_rate <= 25000000 &&
						    audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HP_HD_AAC");
						}
						break;
					default:
						DPRINTF(E_DEBUG, L_METADATA, "AVC profile [%d] not recognized for file %s\n",
							vc->profile, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
						break;
					}
				}
				else
				{
					free(m.dlna_pn);
					m.dlna_pn = NULL;
				}
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is h.264\n", video_stream, basepath);
				break;
			case AV_CODEC_ID_MPEG4:
        			fourcc[0] = vc->codec_tag     & 0xff;
			        fourcc[1] = vc->codec_tag>>8  & 0xff;
			        fourcc[2] = vc->codec_tag>>16 & 0xff;
			        fourcc[3] = vc->codec_tag>>24 & 0xff;
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is MPEG4 [%c%c%c%c/0x%X]\n",
					video_stream, basepath,
					isprint(fourcc[0]) ? fourcc[0] : '_',
					isprint(fourcc[1]) ? fourcc[1] : '_',
					isprint(fourcc[2]) ? fourcc[2] : '_',
					isprint(fourcc[3]) ? fourcc[3] : '_',
					vc->codec_tag);

				if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					m.dlna_pn = malloc(128);
					off = sprintf(m.dlna_pn, "MPEG4_P2_");

					if( ends_with(path, ".3gp") )
					{
						xasprintf(&m.mime, "video/3gpp");
						switch( audio_profile )
						{
							case PROFILE_AUDIO_AAC:
								off += sprintf(m.dlna_pn+off, "3GPP_SP_L0B_AAC");
								break;
							case PROFILE_AUDIO_AMR:
								off += sprintf(m.dlna_pn+off, "3GPP_SP_L0B_AMR");
								break;
							default:
								DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for MPEG4-P2 3GP/0x%X file %s\n",
								        ac->codec_id, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
								break;
						}
					}
					else
					{
						if( ctx->bit_rate <= 1000000 &&
						    audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "MP4_ASP_AAC");
						}
						else if( ctx->bit_rate <= 4000000 &&
						         vc->width  <= 640 &&
						         vc->height <= 480 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "MP4_SP_VGA_AAC");
						}
						else
						{
							DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 video profile! [%dx%d, %dbps]\n",
								vc->width,
								vc->height,
								ctx->bit_rate);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
						}
					}
				}
				break;
			case AV_CODEC_ID_WMV3:
				/* I'm not 100% sure this is correct, but it works on everything I could get my hands on */
				if( vc->extradata_size > 0 )
				{
					if( !((vc->extradata[0] >> 3) & 1) )
						vc->level = 0;
					if( !((vc->extradata[0] >> 6) & 1) )
						vc->profile = 0;
				}
			case AV_CODEC_ID_VC1:
				if( strcmp(ctx->iformat->name, "asf") != 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Skipping DLNA parsing for non-ASF VC1 file %s\n", path);
					break;
				}
				m.dlna_pn = malloc(64);
				off = sprintf(m.dlna_pn, "WMV");
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is VC1\n", video_stream, basepath);
				xasprintf(&m.mime, "video/x-ms-wmv");
				if( (vc->width  <= 176) &&
				    (vc->height <= 144) &&
				    (vc->level == 0) )
				{
					off += sprintf(m.dlna_pn+off, "SPLL_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MP3");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVSPLL/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (vc->width  <= 352) &&
				         (vc->height <= 288) &&
				         (vc->profile == 0) &&
				         (ctx->bit_rate/8 <= 384000) )
				{
					off += sprintf(m.dlna_pn+off, "SPML_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MP3");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVSPML/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (vc->width  <= 720) &&
				         (vc->height <= 576) &&
				         (ctx->bit_rate/8 <= 10000000) )
				{
					off += sprintf(m.dlna_pn+off, "MED_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_WMA_PRO:
							off += sprintf(m.dlna_pn+off, "PRO");
							break;
						case PROFILE_AUDIO_WMA_FULL:
							off += sprintf(m.dlna_pn+off, "FULL");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVMED/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (vc->width  <= 1920) &&
				         (vc->height <= 1080) &&
				         (ctx->bit_rate/8 <= 20000000) )
				{
					off += sprintf(m.dlna_pn+off, "HIGH_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_WMA_PRO:
							off += sprintf(m.dlna_pn+off, "PRO");
							break;
						case PROFILE_AUDIO_WMA_FULL:
							off += sprintf(m.dlna_pn+off, "FULL");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVHIGH/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				break;
			case AV_CODEC_ID_MSMPEG4V3:
				xasprintf(&m.mime, "video/x-msvideo");
			default:
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s [type %d]\n",
					video_stream, basepath, m.resolution, vc->codec_id);
				break;
		}
	}

	if( strcmp(ctx->iformat->name, "asf") == 0 )
	{
		if( readtags((char *)path, &video, &file, "en_US", "asf") == 0 )
		{
			if( video.title && *video.title )
			{
				m.title = escape_tag(trim(video.title), 1);
			}
			if( video.genre && *video.genre )
			{
				m.genre = escape_tag(trim(video.genre), 1);
			}
			if( video.contributor[ROLE_TRACKARTIST] && *video.contributor[ROLE_TRACKARTIST] )
			{
				m.artist = escape_tag(trim(video.contributor[ROLE_TRACKARTIST]), 1);
			}
			if( video.contributor[ROLE_ALBUMARTIST] && *video.contributor[ROLE_ALBUMARTIST] )
			{
				m.creator = escape_tag(trim(video.contributor[ROLE_ALBUMARTIST]), 1);
			}
			else
			{
				m.creator = m.artist;
				free_flags &= ~FLAG_CREATOR;
			}
			if (!m.thumb_data)
			{
				m.thumb_data = video.image;
				m.thumb_size = video.image_size;
			}
		}
	}
	#ifndef NETGEAR
	#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(31<<8)+0)
	else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
	{
		if( ctx->metadata )
		{
			AVDictionaryEntry *tag = NULL;

			//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Metadata:\n");
			while( (tag = av_dict_get(ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) )
			{
				//DEBUG DPRINTF(E_DEBUG, L_METADATA, "  %-16s: %s\n", tag->key, tag->value);
				if( strcmp(tag->key, "title") == 0 )
					m.title = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "genre") == 0 )
					m.genre = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "artist") == 0 )
					m.artist = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "comment") == 0 )
					m.comment = escape_tag(trim(tag->value), 1);
			}
		}
	}
	#endif
	#endif
video_no_dlna:

#ifdef TIVO_SUPPORT
	if( ends_with(path, ".TiVo") && is_tivo_file(path) )
	{
		if( m.dlna_pn )
		{
			free(m.dlna_pn);
			m.dlna_pn = NULL;
		}
		m.mime = realloc(m.mime, 21);
		strcpy(m.mime, "video/x-tivo-mpeg");
	}
#endif

	strcpy(nfo, path);
	ext = strrchr(nfo, '.');
	if( ext )
	{
		strcpy(ext+1, "nfo");
		if( access(nfo, F_OK) == 0 )
		{
			parse_nfo(nfo, &m);
		}
	}

	if( !m.mime )
	{
		if( strcmp(ctx->iformat->name, "avi") == 0 )
			xasprintf(&m.mime, "video/x-msvideo");
		else if( strncmp(ctx->iformat->name, "mpeg", 4) == 0 )
			xasprintf(&m.mime, "video/mpeg");
		else if( strcmp(ctx->iformat->name, "asf") == 0 )
			xasprintf(&m.mime, "video/x-ms-wmv");
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
			if( ends_with(path, ".mov") )
				xasprintf(&m.mime, "video/quicktime");
			else
				xasprintf(&m.mime, "video/mp4");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			xasprintf(&m.mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			xasprintf(&m.mime, "video/x-flv");
		else
			DPRINTF(E_WARN, L_METADATA, "%s: Unhandled format: %s\n", path, ctx->iformat->name);
	}

	if( !m.date )
	{
		m.date = malloc(20);
		modtime = localtime(&file.st_mtime);
		strftime(m.date, 20, "%FT%T", modtime);
	}

	if( !m.title )
		m.title = strdup(name);

        //generate title from path if m2ts-file is located in BDMV/STREAM directory
        if ((ends_with(path, ".m2ts") != 0) && strstr(path, "/BDMV/STREAM/")) {
            m.title = strdup(path); //get title from path  
            strip_ext(m.title); //Remove Extension 

            m.title = replace(m.title, "/BDMV/STREAM/", "#"); //replace BDMV Directory

            char* output;
            if (getStringBetweenDelimiters(m.title, '/', "#", &output) == 0) //get string from last / until BDMV Directory
            {
                 m.title = strdup(output);
                 free(output);
            }
        }
        m.title = replace(m.title, "/", " - "); //Replace / with -   
        DPRINTF(E_DEBUG, L_METADATA, "Title: %s!\n", m.title);
        
        //if DTS audio detected warn, as this is currently not suppport
        //and add [DTS] to title
        if (audio_profile == PROFILE_AUDIO_DTS) {
           DPRINTF(E_WARN, L_METADATA, "DTS Audio found in : %s\n", path);
           asprintf(&m.title, "%s [DTS]", m.title);                           
        }      
        //>

	album_art = find_album_art(path, m.thumb_data, m.thumb_size);
	freetags(&video);
	lav_close(ctx);

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, SIZE, TIMESTAMP, DURATION, DATE, CHANNELS, BITRATE, SAMPLERATE, RESOLUTION,"
	                   "  TITLE, CREATOR, ARTIST, GENRE, COMMENT, DLNA_PN, MIME, ALBUM_ART) "
	                   "VALUES"
	                   " (%Q, %lld, %lld, %Q, %Q, %u, %u, %u, %Q, '%q', %Q, %Q, %Q, %Q, %Q, '%q', %lld);",
	                   path, (long long)file.st_size, (long long)file.st_mtime, m.duration,
	                   m.date, m.channels, m.bitrate, m.frequency, m.resolution,
			   m.title, m.creator, m.artist, m.genre, m.comment, m.dlna_pn,
                           m.mime, album_art);
	if( ret != SQLITE_OK )
	{
		DPRINTF(E_ERROR, L_METADATA, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
		check_for_captions(path, ret);

		if ( GETFLAG(CACHE_METADATA_MASK) )
		{
			write_metadata_file(path, &file, &m, album_art);
		}
	}
	free_metadata(&m, free_flags);
	free(path_cpy);

	return ret;
}


///////////////////////////////////////////////////////////////////////////////
/////   following code based on lsdvd.c of lsdvd 0.16
////    http://sourceforge.net/projects/lsdvd/
//
//  Copyright (C) 2003  EFF
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2 as
//  published by the Free Software Foundation;
//
//  2003	by Chris Phillips
//  2003-04-19  Cleanups get_title_name, added dvdtime2msec, added helper macros,
//			  output info structures in form of a Perl module, by Henk Vergonet.
//
///////////////////////////////////////////////////////////////////////////////

double frames_per_s[4] = {-1.0, 25.00, -1.0, 29.97};
char *video_format[2] = {"NTSC", "PAL"};
char *aspect_ratio[4] = {"4/3", "16/9", "\"?:?\"", "16/9"};
int video_height[4] = {480, 576, 0, 576};
int video_width[4]  = {720, 704, 352, 352};
char *mpeg_version[2] = {"mpeg1", "mpeg2"};
char *audio_format[7] = {"ac3", "?", "mpeg1", "mpeg2", "lpcm ", "sdds ", "dts"};
int sample_freq[2]  = {48000, 48000};

int dvdtime2msec(dvd_time_t *dt)
{
	double fps = frames_per_s[(dt->frame_u & 0xc0) >> 6];
	long   ms;
	ms  = (((dt->hour &   0xf0) >> 3) * 5 + (dt->hour   & 0x0f)) * 3600000;
	ms += (((dt->minute & 0xf0) >> 3) * 5 + (dt->minute & 0x0f)) * 60000;
	ms += (((dt->second & 0xf0) >> 3) * 5 + (dt->second & 0x0f)) * 1000;

	if(fps > 0)
	ms += ((dt->frame_u & 0x30) >> 3) * 5 + (dt->frame_u & 0x0f) * 1000.0 / fps;

	return ms;
}

int64_t
GetIFOMetadata(const char *path, char *name, char *browse_dir_id, const char* base, const char *class, const char *parentID, int object)
{
	struct stat file;
	int ret = 0, i;
	struct tm *modtime;
	enum audio_profiles audio_profile = PROFILE_AUDIO_UNKNOWN;
	int64_t album_art = 0;
	//struct song_metadata video;
        
	uint32_t free_flags = 0xFFFFFFFF;
        
        int audio_streams = 0;    

	//memset(&video, '\0', sizeof(video));
        
        //dvdread variables
        dvd_reader_t *dvd;
        ifo_handle_t *ifo_info, **ifo;
        pgcit_t *vts_pgcit;
	vtsi_mat_t *vtsi_mat;
	audio_attr_t *audio_attr;
	video_attr_t *video_attr;
        pgc_t *pgc;
	int j, titles, vts_ttn, title_set_nr;   
        int chapters = 0;
        int start_sector = 0;
     

	DPRINTF(E_DEBUG, L_METADATA, "Parsing %s...\n", path);
	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);

        //Remove VIDEO_TS.IFO filename from path, as libdvread expects a folder, Warning: replace is case sensitive
        char *real_path = replace(path, "VIDEO_TS.IFO", "");            
        
        //open dvd folder
	dvd = DVDOpen(real_path);
	if( !dvd ) {
		DPRINTF(E_WARN, L_METADATA, "Can't open disc %s!\n", path);
		return 0;
	}        
        
        //open main ifo file
	DPRINTF(E_DEBUG, L_METADATA, "Accessing main ifo %s...\n", path);
        ifo_info = ifoOpen(dvd, 0);
	if ( !ifo_info ) {
                DPRINTF(E_WARN, L_METADATA, "Can't open main ifo %s!\n", path);
                DVDClose(dvd);
		return 0;
	}
        
	ifo = (ifo_handle_t **)malloc((ifo_info->vts_atrt->nr_of_vtss + 1) * sizeof(ifo_handle_t *));

        //open IFO files
	for (i=1; i <= ifo_info->vts_atrt->nr_of_vtss; i++) {
		DPRINTF(E_DEBUG, L_METADATA, "Accessing ifo %d of %s...\n", i, path);
                ifo[i] = ifoOpen(dvd, i);
		if ( !ifo[i] ) {
                        DPRINTF(E_WARN, L_METADATA, "Can't open ifo nr %d of %s!\n", i, path);
                        
                        //cleanup
                        int x = 1;
                        for (x=1; x <= i; x++) { 
                            ifoClose(ifo[x]);	
                        }                        
                        ifoClose(ifo_info);
                        DVDClose(dvd);
                        
			return 0;
		}
	}
	titles = ifo_info->tt_srpt->nr_of_srpts;        

        //Analyze Titles
        if (titles == 0) {
           DPRINTF(E_WARN, L_METADATA, "No titles found in ifo!\n");
           
           //cleanup
           for (i=1; i <= ifo_info->vts_atrt->nr_of_vtss; i++) { ifoClose(ifo[i]);}
           ifoClose(ifo_info);
           DVDClose(dvd);
           return 0;
        }
                
        DPRINTF(E_DEBUG, L_METADATA, "VTS Titles : %d\n", titles);                        
        j = 0;
          
        //Get File Information 
	DPRINTF(E_DEBUG, L_METADATA, "Reading VOB for ifo %d of %s...\n", i, path);
        dvd_file_t* vobs_file = NULL;          
        vobs_file = DVDOpenFile(dvd, 1, DVD_READ_TITLE_VOBS);
        if (vobs_file == 0) {
            DPRINTF(E_WARN, L_METADATA, "Can't get file infos from Title %i of %s!\n", j, path);
            
            //cleanup
            DVDCloseFile(vobs_file);
            for (i=1; i <= ifo_info->vts_atrt->nr_of_vtss; i++) { ifoClose(ifo[i]);}
            ifoClose(ifo_info);
            DVDClose(dvd);
            return 0;
        }
        long vob_size = DVDFileSize(vobs_file)*DVD_VIDEO_LB_LEN;
        DVDCloseFile(vobs_file);
          
        DPRINTF(E_DEBUG, L_METADATA, "Size of Title %i: %ld bytes\n", j + 1, vob_size );        
          
        //Analyze Titles
        for (j=0; j < titles; j++) {
            if (ifo[ifo_info->tt_srpt->title[j].title_set_nr]->vtsi_mat) {
                DPRINTF(E_DEBUG, L_METADATA, "Analyzing Title %i\n", j+1);                   
                
                //Varabiles for metadata
                metadata_t m;
        	memset(&m, '\0', sizeof(m));  
                
                //Get Format
		vtsi_mat     = ifo[ifo_info->tt_srpt->title[j].title_set_nr]->vtsi_mat;
		vts_pgcit    = ifo[ifo_info->tt_srpt->title[j].title_set_nr]->vts_pgcit;
		vts_ttn      = ifo_info->tt_srpt->title[j].vts_ttn;
		title_set_nr = ifo_info->tt_srpt->title[j].title_set_nr;
                pgc          = vts_pgcit->pgci_srp[ifo[title_set_nr]->vts_ptt_srpt->title[vts_ttn - 1].ptt[0].pgcn - 1].pgc;
		video_attr   = &vtsi_mat->vts_video_attr;                
                
                //Get Chapters               
                chapters     = ifo_info->tt_srpt->title[j].nr_of_ptts;                
                int start_cell = pgc->program_map[0];
                int end_cell   = pgc->nr_of_cells;
                
                start_sector = pgc->cell_playback[start_cell - 1].first_sector;
                int end_sector   = pgc->cell_playback[end_cell - 1].last_sector;
                
                DPRINTF(E_DEBUG, L_METADATA, "Chapters: %i, Cells: %i, Start Sector: %i, End Sector: %i\n", chapters, end_cell, start_sector, end_sector);
                
                if (titles > 1) {
                    //calculate file size from cell sizes
                    vob_size = (end_sector-start_sector)*DVD_VIDEO_LB_LEN;
                    DPRINTF(E_DEBUG, L_METADATA, "Size of Chapters of Title %i : %ld\n",j+1, vob_size);
                }
                                
                int fps = frames_per_s[(pgc->playback_time.frame_u & 0xc0) >> 6];
		char *aspect = aspect_ratio[video_attr->display_aspect_ratio];
                int width = video_width[video_attr->picture_size];
                int height = video_height[video_attr->video_format];                                
                xasprintf(&m.resolution, "%dx%d", width, height);
                //DPRINTF(E_DEBUG, L_METADATA, "VTS[%d] %d x %d\n", title_set_nr, width, height);                
                
                float length = dvdtime2msec(&pgc->playback_time)/1000.0;
                if( length > 0 ) {
                        int duration, hours, min, sec, ms;
			duration = (int)(length);
			hours = (int)(duration / 3600);
			min = (int)(duration / 60 % 60);
			sec = (int)(duration % 60);
			ms = (int)(duration % 1000); //Warning: not accurate
			xasprintf(&m.duration, "%d:%02d:%02d.%03d", hours, min, sec, ms);
		}   
                   
                //Set Video Format to MPEG2
                //we asume MPEG1=MPEG2
		xasprintf(&m.mime, "video/mpeg");   
                
		m.dlna_pn = malloc(64);
                int off;                
                off = sprintf(m.dlna_pn, "MPEG_");				
		off += sprintf(m.dlna_pn+off, "PS_");                
                
		if(strcmp(video_format[video_attr->video_format],"PAL") == 0)
		    off += sprintf(m.dlna_pn+off, "PAL");
		else
		    off += sprintf(m.dlna_pn+off, "NTSC");              
                
                DPRINTF(E_DEBUG, L_METADATA, "Title [%d] VTS[%d] Video, Length: %s, Format: %s, fps: %i, aspect %s, Resolution: %s\n", j+1, title_set_nr, m.duration, m.dlna_pn, fps, aspect, m.resolution);                
                
                //Audio
                audio_streams = vtsi_mat->nr_of_vts_audio_streams;                                              
                DPRINTF(E_DEBUG, L_METADATA, "Title [%d] VTS[%d] Audio Streams: %i\n", j+1, title_set_nr, audio_streams );
                
                if (audio_streams > 0) {                                    
                    //TODO: support for multiple audio streams
                    i = 0;
                 
                    audio_attr = &vtsi_mat->vts_audio_attr[i];
		    int frequency = sample_freq[audio_attr->sample_frequency];
		    int channels = audio_attr->channels+1;
                    char *a_format = audio_format[audio_attr->audio_format];                          
                                     
		    if (strcmp(a_format, "ac3") == 0) {
                       audio_profile = PROFILE_AUDIO_AC3;
                       a_format = "AC3";
                    }
                    else if (strcmp(a_format, "dts") == 0) { 
		       audio_profile = PROFILE_AUDIO_DTS;                                
                       a_format = "DTS";
                    }
                    else if (strcmp(a_format, "mpeg1") == 0) { 
                       audio_profile = PROFILE_AUDIO_MP2;
                       a_format = "MP2";
                       DPRINTF(E_INFO, L_METADATA, "Detected audio format MPEG1 detected, handling as MPEG2\n");                      
                    }
                    else if (strcmp(a_format, "mpeg2") == 0) { 
                       audio_profile = PROFILE_AUDIO_MP2;
                       a_format = "MP2";
                    }
                    else if (strcmp(a_format, "lpcm") == 0) { 
 		       audio_profile = PROFILE_AUDIO_PCM;
                       a_format = "PCM";
                    }
                    else {
        	       DPRINTF(E_WARN, L_METADATA, "Unhandled audio format [0x%X], ignoring title %d\n", audio_attr->audio_format, j+1);     
                       free_metadata(&m, free_flags);                     
                       continue;
		    }
		    m.channels = channels;
		    m.frequency = frequency;
                  
                    DPRINTF(E_DEBUG, L_METADATA, "Title [%d] VTS[%d] Audio [%i]: Format: %s, Channels: %i, Frequency: %i\n", j+1, title_set_nr, i, a_format, channels, frequency);
		
                } else {
                    DPRINTF(E_WARN, L_METADATA, "Ignoring title %d without audio in %s\n", j+1, path);                
                    free_metadata(&m, free_flags);                     
                    continue;
                }     
            
                //Add Video to Database
                if( !m.date ) {
		    m.date = malloc(20);
		    modtime = localtime(&file.st_mtime);
		    strftime(m.date, 20, "%FT%T", modtime);
	        }
            
                if( !m.title ) m.title = strdup(name);         
                
                //generate vob title from path                  
                m.title = strdup(path);                             //get title from path  
                strip_ext(m.title);                                 //Remove Extension 
                    
                if (strstr(m.title, "/VIDEO_TS/")) {
                    m.title = replace(m.title, "/VIDEO_TS/", "#");   //replace VIDEO_TS Directory

                    char* output;                    
                    if(getStringBetweenDelimiters(m.title, '/', "#", &output) == 0) //get string from last / until VIDEO_TS Directory
                    {
                      m.title = strdup(output);
                      free(output);
                    }
                }
                m.title = replace(m.title, "/", " - ");          //Replace / with -                                      
                
                //if more than 1 titles append VTS id if not the only title
                if (titles > 1 ) {                
                     xasprintf(&m.title, "%s - %i", m.title, j+1);                    
                }                
                        
                //if DTS audio detected warn, as this is currently not suppport
                //and add [DTS] to title
                if (audio_profile == PROFILE_AUDIO_DTS) {
                     DPRINTF(E_WARN, L_METADATA, "DTS Audio found in : %s\n", path);
                     asprintf(&m.title, "%s [DTS]", m.title);                           
                }           
        
                DPRINTF(E_DEBUG, L_METADATA, "Title: %s!\n", m.title);

/* */
                //album_art = find_album_art(path, video.image, video.image_size);
                album_art = find_album_art(path, NULL, 0);
                //freetags(&video);
                
		//TODO(jdef) support metadata cache file (like we do for video)?
                //hint: Save VOB title start sector in column TRACK
                DPRINTF(E_DEBUG, L_METADATA, "Save VOB Title %i start sector\n", j+1);                   
                ret = sql_exec(db, "INSERT into DETAILS"
                                   " (PATH, SIZE, TIMESTAMP, DURATION, DATE, CHANNELS, BITRATE, SAMPLERATE, RESOLUTION,"
                                   "  TITLE, CREATOR, ARTIST, GENRE, COMMENT, DLNA_PN, MIME, ALBUM_ART, TRACK) " 
                                   "VALUES"
                                   " (%Q, %ld, %lld, '%s', %Q, %d, %d, %d, %Q, '%q', %Q, %Q, %Q, %Q, %Q, '%q', %lld, %d);",
                                   path, vob_size, (long long)file.st_mtime, m.duration,
                                   m.date, m.channels, m.bitrate, m.frequency, m.resolution,
                                   m.title, m.creator, m.artist, m.genre, m.comment, m.dlna_pn,
                                   m.mime, album_art, start_sector);
                if( ret != SQLITE_OK ) {
                        DPRINTF(E_WARN, L_METADATA, "Error inserting details for '%s'!\n", path);
                        ret = 0;
                        
                	free_metadata(&m, free_flags);                     
                        continue;
                }
                else {
                	DPRINTF(E_DEBUG, L_METADATA, "Checking Title %i for captions\n", j+1);                   
                        ret = sqlite3_last_insert_rowid(db);
                        check_for_captions(path, ret);
                }
                
                //add title as object to database
                DPRINTF(E_DEBUG, L_METADATA, "Add Title %i as object to database\n", j+1);                   
                char objectID[64];
                sprintf(objectID, "%s%s$%X", browse_dir_id, parentID, object + j);

                //DPRINTF(E_DEBUG, L_METADATA, "inserting details for id '%i'!\n", ret);

                //insert
                //DPRINTF(E_DEBUG, L_METADATA, "INSERT into OBJECTS"
                //" (OBJECT_ID, PARENT_ID, CLASS, DETAIL_ID, NAME) "
                //"VALUES"
                //" ('%s', '%s%s', '%s', %lld, '%s')\n",
                //objectID, browse_dir_id, parentID, class, ret, m.title);

                int r1 = sql_exec(db, "INSERT into OBJECTS"
                        " (OBJECT_ID, PARENT_ID, CLASS, DETAIL_ID, NAME) "
                        "VALUES"
                        " ('%s', '%s%s', '%s', %i, '%q')",
                        objectID, browse_dir_id, parentID, class, ret, m.title);
                
                if (r1 != SQLITE_OK) {
                    DPRINTF(E_WARN, L_METADATA, "Error inserting details for '%s'!\n", path);
                }

                //DPRINTF(E_DEBUG, L_METADATA, "INSERT into OBJECTS"
                //" (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
                //"VALUES"
                //" ('%s%s$%X', '%s%s', '%s', '%s', %i, '%s')\n",
                //base, parentID, object+j, base, parentID, objectID, class, ret, name);

                DPRINTF(E_DEBUG, L_METADATA, "Add(2) Title %i as object to database\n", j+1);                   
                int r2 = sql_exec(db, "INSERT into OBJECTS"
                        " (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
                        "VALUES"
                        " ('%s%s$%X', '%s%s', '%s', '%s', %i, '%q')",
                        base, parentID, object + j, base, parentID, objectID, class, ret, name);

                if (r2 != SQLITE_OK) {
                    DPRINTF(E_WARN, L_METADATA, "Error inserting details for '%s'!\n", path);
                }                       
/* */
            
                //free metadata
                free_metadata(&m, free_flags);                     
                DPRINTF(E_DEBUG, L_METADATA, "Finished analyzing Title %i\n", j+1);                   
            }
        } 
        
        //cleanup
        for (i=1; i <= ifo_info->vts_atrt->nr_of_vtss; i++) { ifoClose(ifo[i]);}
        ifoClose(ifo_info);
	DVDClose(dvd);
 
	return ret;
}
