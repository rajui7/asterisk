/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2013, Digium, Inc.
 *
 * Kinsey Moore <kmoore@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 * \brief Sound file format and description indexer.
 */

#ifndef _ASTERISK_SOUNDS_INDEX_H
#define _ASTERISK_SOUNDS_INDEX_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*!
 * \brief Object representing a media index
 */
struct ast_media_index;

/*!
 * \brief Reload the sounds index
 *
 * \retval zero on success
 * \retval non-zero on failure
 */
int ast_sounds_reindex(void);

/*!
 * \brief Get the sounds index
 *
 * \retval sounds index (must be ao2_cleanup()'ed)
 * \retval NULL on failure
 */
struct ast_media_index *ast_sounds_get_index(void);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _ASTERISK_SOUNDS_INDEX_H */
