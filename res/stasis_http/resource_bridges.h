/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2012 - 2013, Digium, Inc.
 *
 * David M. Lee, II <dlee@digium.com>
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
 *
 * \brief Generated file - declares stubs to be implemented in
 * res/stasis_http/resource_bridges.c
 *
 * Bridge resources
 *
 * \author David M. Lee, II <dlee@digium.com>
 */

/*
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * !!!!!                               DO NOT EDIT                        !!!!!
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * This file is generated by a mustache template. Please see the original
 * template in rest-api-templates/stasis_http_resource.h.mustache
 */

#ifndef _ASTERISK_RESOURCE_BRIDGES_H
#define _ASTERISK_RESOURCE_BRIDGES_H

#include "asterisk/stasis_http.h"

/*! \brief Argument struct for stasis_http_get_bridges() */
struct ast_get_bridges_args {
};
/*!
 * \brief List active bridges.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void stasis_http_get_bridges(struct ast_variable *headers, struct ast_get_bridges_args *args, struct stasis_http_response *response);
/*! \brief Argument struct for stasis_http_new_bridge() */
struct ast_new_bridge_args {
	/*! \brief Type of bridge to create. */
	const char *type;
};
/*!
 * \brief Create a new bridge.
 *
 * This bridge persists until it has been shut down, or Asterisk has been shut down.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void stasis_http_new_bridge(struct ast_variable *headers, struct ast_new_bridge_args *args, struct stasis_http_response *response);
/*! \brief Argument struct for stasis_http_get_bridge() */
struct ast_get_bridge_args {
	/*! \brief Bridge's id */
	const char *bridge_id;
};
/*!
 * \brief Get bridge details.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void stasis_http_get_bridge(struct ast_variable *headers, struct ast_get_bridge_args *args, struct stasis_http_response *response);
/*! \brief Argument struct for stasis_http_delete_bridge() */
struct ast_delete_bridge_args {
	/*! \brief Bridge's id */
	const char *bridge_id;
};
/*!
 * \brief Shut down a bridge bridge.
 *
 * If any channels are in this bridge, they will be removed and resume whatever they were doing beforehand.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void stasis_http_delete_bridge(struct ast_variable *headers, struct ast_delete_bridge_args *args, struct stasis_http_response *response);
/*! \brief Argument struct for stasis_http_add_channel_to_bridge() */
struct ast_add_channel_to_bridge_args {
	/*! \brief Bridge's id */
	const char *bridge_id;
	/*! \brief Channel's id */
	const char *channel;
};
/*!
 * \brief Add a channel to a bridge.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void stasis_http_add_channel_to_bridge(struct ast_variable *headers, struct ast_add_channel_to_bridge_args *args, struct stasis_http_response *response);
/*! \brief Argument struct for stasis_http_remove_channel_from_bridge() */
struct ast_remove_channel_from_bridge_args {
	/*! \brief Bridge's id */
	const char *bridge_id;
	/*! \brief Channel's id */
	const char *channel;
};
/*!
 * \brief Remove a channel from a bridge.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void stasis_http_remove_channel_from_bridge(struct ast_variable *headers, struct ast_remove_channel_from_bridge_args *args, struct stasis_http_response *response);
/*! \brief Argument struct for stasis_http_record_bridge() */
struct ast_record_bridge_args {
	/*! \brief Bridge's id */
	const char *bridge_id;
	/*! \brief Recording's filename */
	const char *name;
	/*! \brief Maximum duration of the recording, in seconds. 0 for no limit. */
	int max_duration_seconds;
	/*! \brief Maximum duration of silence, in seconds. 0 for no limit. */
	int max_silence_seconds;
	/*! \brief If true, and recording already exists, append to recording. */
	int append;
	/*! \brief Play beep when recording begins */
	int beep;
	/*! \brief DTMF input to terminate recording. */
	const char *terminate_on;
};
/*!
 * \brief Start a recording.
 *
 * This records the mixed audio from all channels participating in this bridge.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void stasis_http_record_bridge(struct ast_variable *headers, struct ast_record_bridge_args *args, struct stasis_http_response *response);

#endif /* _ASTERISK_RESOURCE_BRIDGES_H */
