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
 * \brief Implementation for ARI stubs.
 *
 * \author David M. Lee, II <dlee@digium.com>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "resource_bridges.h"
#include "asterisk/stasis.h"
#include "asterisk/stasis_bridges.h"
#include "asterisk/stasis_app.h"
#include "asterisk/stasis_app_playback.h"
#include "asterisk/stasis_app_recording.h"
#include "asterisk/stasis_channels.h"
#include "asterisk/core_unreal.h"
#include "asterisk/channel.h"
#include "asterisk/bridge.h"
#include "asterisk/format_cap.h"
#include "asterisk/file.h"
#include "asterisk/musiconhold.h"

/*!
 * \brief Finds a bridge, filling the response with an error, if appropriate.
 *
 * \param[out] response Response to fill with an error if control is not found.
 * \param bridge_id ID of the bridge to lookup.
 *
 * \return Bridget.
 * \return \c NULL if bridge does not exist.
 */
static struct ast_bridge *find_bridge(
	struct ast_ari_response *response,
	const char *bridge_id)
{
	RAII_VAR(struct ast_bridge *, bridge, NULL, ao2_cleanup);

	ast_assert(response != NULL);

	bridge = stasis_app_bridge_find_by_id(bridge_id);
	if (bridge == NULL) {
		RAII_VAR(struct ast_bridge_snapshot *, snapshot,
			ast_bridge_snapshot_get_latest(bridge_id), ao2_cleanup);
		if (!snapshot) {
			ast_ari_response_error(response, 404, "Not found",
				"Bridge not found");
			return NULL;
		}

		ast_ari_response_error(response, 409, "Conflict",
			"Bridge not in Stasis application");
		return NULL;
	}

	ao2_ref(bridge, +1);
	return bridge;
}

/*!
 * \brief Finds the control object for a channel, filling the response with an
 * error, if appropriate.
 * \param[out] response Response to fill with an error if control is not found.
 * \param channel_id ID of the channel to lookup.
 * \return Channel control object.
 * \return \c NULL if control object does not exist.
 */
static struct stasis_app_control *find_channel_control(
	struct ast_ari_response *response,
	const char *channel_id)
{
	RAII_VAR(struct stasis_app_control *, control, NULL, ao2_cleanup);

	ast_assert(response != NULL);

	control = stasis_app_control_find_by_channel_id(channel_id);
	if (control == NULL) {
		/* Distinguish between 400 and 422 errors */
		RAII_VAR(struct ast_channel_snapshot *, snapshot, NULL,
			ao2_cleanup);
		snapshot = ast_channel_snapshot_get_latest(channel_id);
		if (snapshot == NULL) {
			ast_log(LOG_DEBUG, "Couldn't find '%s'\n", channel_id);
			ast_ari_response_error(response, 400, "Bad Request",
				"Channel not found");
			return NULL;
		}

		ast_log(LOG_DEBUG, "Found non-stasis '%s'\n", channel_id);
		ast_ari_response_error(response, 422, "Unprocessable Entity",
			"Channel not in Stasis application");
		return NULL;
	}

	ao2_ref(control, +1);
	return control;
}

struct control_list {
	size_t count;
	struct stasis_app_control *controls[];
};

static void control_list_dtor(void *obj) {
	struct control_list *list = obj;
	size_t i;

	for (i = 0; i < list->count; ++i) {
		ao2_cleanup(list->controls[i]);
		list->controls[i] = NULL;
	}
}

static struct control_list *control_list_create(struct ast_ari_response *response, size_t count, const char **channels) {
	RAII_VAR(struct control_list *, list, NULL, ao2_cleanup);
	size_t i;

	if (count == 0 || !channels) {
		ast_ari_response_error(response, 400, "Bad Request", "Missing parameter channel");
		return NULL;
	}

	list = ao2_alloc(sizeof(*list) + count * sizeof(list->controls[0]), control_list_dtor);
	if (!list) {
		ast_ari_response_alloc_failed(response);
		return NULL;
	}

	for (i = 0; i < count; ++i) {
		if (ast_strlen_zero(channels[i])) {
			continue;
		}
		list->controls[list->count] =
			find_channel_control(response, channels[i]);
		if (!list->controls[list->count]) {
			/* response filled in by find_channel_control() */
			return NULL;
		}
		++list->count;
	}

	if (list->count == 0) {
		ast_ari_response_error(response, 400, "Bad Request", "Missing parameter channel");
		return NULL;
	}

	ao2_ref(list, +1);
	return list;
}

void ast_ari_add_channel_to_bridge(struct ast_variable *headers, struct ast_add_channel_to_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge *, bridge, find_bridge(response, args->bridge_id), ao2_cleanup);
	RAII_VAR(struct control_list *, list, NULL, ao2_cleanup);
	size_t i;

	if (!bridge) {
		/* Response filled in by find_bridge() */
		return;
	}

	list = control_list_create(response, args->channel_count, args->channel);
	if (!list) {
		/* Response filled in by control_list_create() */
		return;
	}

	for (i = 0; i < list->count; ++i) {
		stasis_app_control_clear_roles(list->controls[i]);
		if (!ast_strlen_zero(args->role)) {
			if (stasis_app_control_add_role(list->controls[i], args->role)) {
				ast_ari_response_alloc_failed(response);
				return;
			}
		}
	}

	for (i = 0; i < list->count; ++i) {
		stasis_app_control_add_channel_to_bridge(list->controls[i], bridge);
	}

	ast_ari_response_no_content(response);
}

void ast_ari_remove_channel_from_bridge(struct ast_variable *headers, struct ast_remove_channel_from_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge *, bridge, find_bridge(response, args->bridge_id), ao2_cleanup);
	RAII_VAR(struct control_list *, list, NULL, ao2_cleanup);
	size_t i;

	if (!bridge) {
		/* Response filled in by find_bridge() */
		return;
	}

	list = control_list_create(response, args->channel_count, args->channel);
	if (!list) {
		/* Response filled in by control_list_create() */
		return;
	}

	/* Make sure all of the channels are in this bridge */
	for (i = 0; i < list->count; ++i) {
		if (stasis_app_get_bridge(list->controls[i]) != bridge) {
			ast_log(LOG_WARNING, "Channel %s not in bridge %s\n",
				args->channel[i], args->bridge_id);
			ast_ari_response_error(response, 422,
				"Unprocessable Entity",
				"Channel not in this bridge");
			return;
		}
	}

	/* Now actually remove it */
	for (i = 0; i < list->count; ++i) {
		stasis_app_control_remove_channel_from_bridge(list->controls[i],
			bridge);
	}

	ast_ari_response_no_content(response);
}

struct bridge_channel_control_thread_data {
	struct ast_channel *bridge_channel;
	struct stasis_app_control *control;
};

static void *bridge_channel_control_thread(void *data)
{
	struct bridge_channel_control_thread_data *thread_data = data;
	struct ast_channel *bridge_channel = thread_data->bridge_channel;
	struct stasis_app_control *control = thread_data->control;

	RAII_VAR(struct ast_callid *, callid, ast_channel_callid(bridge_channel), ast_callid_cleanup);

	if (callid) {
		ast_callid_threadassoc_add(callid);
	}

	ast_free(thread_data);
	thread_data = NULL;

	stasis_app_control_execute_until_exhausted(bridge_channel, control);

	ast_hangup(bridge_channel);
	ao2_cleanup(control);
	return NULL;
}

static struct ast_channel *prepare_bridge_media_channel(const char *type)
{
	RAII_VAR(struct ast_format_cap *, cap, NULL, ast_format_cap_destroy);
	struct ast_format format;

	cap = ast_format_cap_alloc_nolock();
	if (!cap) {
		return NULL;
	}

	ast_format_cap_add(cap, ast_format_set(&format, AST_FORMAT_SLINEAR, 0));

	if (!cap) {
		return NULL;
	}

	return ast_request(type, cap, NULL, "ARI", NULL);
}

void ast_ari_play_on_bridge(struct ast_variable *headers, struct ast_play_on_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge *, bridge, find_bridge(response, args->bridge_id), ao2_cleanup);
	RAII_VAR(struct ast_channel *, play_channel, NULL, ast_hangup);
	RAII_VAR(struct stasis_app_control *, control, NULL, ao2_cleanup);
	RAII_VAR(struct ast_channel_snapshot *, snapshot, NULL, ao2_cleanup);
	RAII_VAR(struct stasis_app_playback *, playback, NULL, ao2_cleanup);
	RAII_VAR(char *, playback_url, NULL, ast_free);
	RAII_VAR(struct ast_json *, json, NULL, ast_json_unref);

	struct bridge_channel_control_thread_data *thread_data;
	const char *language;
	pthread_t threadid;

	ast_assert(response != NULL);

	if (!bridge) {
		return;
	}

	if (!(play_channel = prepare_bridge_media_channel("Announcer"))) {
		ast_ari_response_error(
			response, 500, "Internal Error", "Could not create playback channel");
		return;
	}
	ast_debug(1, "Created announcer channel '%s'\n", ast_channel_name(play_channel));

	if (ast_unreal_channel_push_to_bridge(play_channel, bridge,
		AST_BRIDGE_CHANNEL_FLAG_IMMOVABLE | AST_BRIDGE_CHANNEL_FLAG_LONELY)) {
		ast_ari_response_error(
			response, 500, "Internal Error", "Failed to put playback channel into the bridge");
		return;
	}

	control = stasis_app_control_create(play_channel);
	if (control == NULL) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	snapshot = stasis_app_control_get_snapshot(control);
	if (!snapshot) {
		ast_ari_response_error(
			response, 500, "Internal Error", "Failed to get control snapshot");
		return;
	}

	language = S_OR(args->lang, snapshot->language);

	playback = stasis_app_control_play_uri(control, args->media, language,
		args->bridge_id, STASIS_PLAYBACK_TARGET_BRIDGE, args->skipms,
		args->offsetms);

	if (!playback) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	ast_asprintf(&playback_url, "/playback/%s",
		stasis_app_playback_get_id(playback));

	if (!playback_url) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	json = stasis_app_playback_to_json(playback);
	if (!json) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	/* Give play_channel and control reference to the thread data */
	thread_data = ast_calloc(1, sizeof(*thread_data));
	if (!thread_data) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	thread_data->bridge_channel = play_channel;
	thread_data->control = control;

	if (ast_pthread_create_detached(&threadid, NULL, bridge_channel_control_thread, thread_data)) {
		ast_ari_response_alloc_failed(response);
		ast_free(thread_data);
		return;
	}

	/* These are owned by the other thread now, so we don't want RAII_VAR disposing of them. */
	play_channel = NULL;
	control = NULL;

	ast_ari_response_created(response, playback_url, json);
}

void ast_ari_record_bridge(struct ast_variable *headers, struct ast_record_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge *, bridge, find_bridge(response, args->bridge_id), ao2_cleanup);
	RAII_VAR(struct ast_channel *, record_channel, NULL, ast_hangup);
	RAII_VAR(struct stasis_app_control *, control, NULL, ao2_cleanup);
	RAII_VAR(struct stasis_app_recording *, recording, NULL, ao2_cleanup);
	RAII_VAR(char *, recording_url, NULL, ast_free);
	RAII_VAR(struct ast_json *, json, NULL, ast_json_unref);
	RAII_VAR(struct stasis_app_recording_options *, options, NULL, ao2_cleanup);
	RAII_VAR(char *, uri_encoded_name, NULL, ast_free);

	size_t uri_name_maxlen;
	struct bridge_channel_control_thread_data *thread_data;
	pthread_t threadid;

	ast_assert(response != NULL);

	if (bridge == NULL) {
		return;
	}

	if (!(record_channel = prepare_bridge_media_channel("Recorder"))) {
		ast_ari_response_error(
			response, 500, "Internal Server Error", "Failed to create recording channel");
		return;
	}

	if (ast_unreal_channel_push_to_bridge(record_channel, bridge,
		AST_BRIDGE_CHANNEL_FLAG_IMMOVABLE | AST_BRIDGE_CHANNEL_FLAG_LONELY)) {
		ast_ari_response_error(
			response, 500, "Internal Error", "Failed to put recording channel into the bridge");
		return;
	}

	control = stasis_app_control_create(record_channel);
	if (control == NULL) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	options = stasis_app_recording_options_create(args->name, args->format);
	if (options == NULL) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	options->max_silence_seconds = args->max_silence_seconds;
	options->max_duration_seconds = args->max_duration_seconds;
	options->terminate_on =
		stasis_app_recording_termination_parse(args->terminate_on);
	options->if_exists =
		stasis_app_recording_if_exists_parse(args->if_exists);
	options->beep = args->beep;

	recording = stasis_app_control_record(control, options);
	if (recording == NULL) {
		switch(errno) {
		case EINVAL:
			/* While the arguments are invalid, we should have
			 * caught them prior to calling record.
			 */
			ast_ari_response_error(
				response, 500, "Internal Server Error",
				"Error parsing request");
			break;
		case EEXIST:
			ast_ari_response_error(response, 409, "Conflict",
				"Recording '%s' already in progress",
				args->name);
			break;
		case ENOMEM:
			ast_ari_response_alloc_failed(response);
			break;
		case EPERM:
			ast_ari_response_error(
				response, 400, "Bad Request",
				"Recording name invalid");
			break;
		default:
			ast_log(LOG_WARNING,
				"Unrecognized recording error: %s\n",
				strerror(errno));
			ast_ari_response_error(
				response, 500, "Internal Server Error",
				"Internal Server Error");
			break;
		}
		return;
	}

	uri_name_maxlen = strlen(args->name) * 3;
	uri_encoded_name = ast_malloc(uri_name_maxlen);
	if (!uri_encoded_name) {
		ast_ari_response_alloc_failed(response);
		return;
	}
	ast_uri_encode(args->name, uri_encoded_name, uri_name_maxlen, ast_uri_http);

	ast_asprintf(&recording_url, "/recordings/live/%s", uri_encoded_name);
	if (!recording_url) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	json = stasis_app_recording_to_json(recording);
	if (!json) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	thread_data = ast_calloc(1, sizeof(*thread_data));
	if (!thread_data) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	thread_data->bridge_channel = record_channel;
	thread_data->control = control;

	if (ast_pthread_create_detached(&threadid, NULL, bridge_channel_control_thread, thread_data)) {
		ast_ari_response_alloc_failed(response);
		ast_free(thread_data);
		return;
	}

	/* These are owned by the other thread now, so we don't want RAII_VAR disposing of them. */
	record_channel = NULL;
	control = NULL;

	ast_ari_response_created(response, recording_url, json);
}

void ast_ari_moh_start_bridge(struct ast_variable *headers, struct ast_moh_start_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge *, bridge, find_bridge(response, args->bridge_id), ao2_cleanup);
	struct ast_channel *moh_channel;
	const char *moh_class = args->moh_class;

	if (!bridge) {
		/* The response is provided by find_bridge() */
		return;
	}

	moh_channel = stasis_app_bridge_moh_channel(bridge);
	if (!moh_channel) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	ast_moh_start(moh_channel, moh_class, NULL);

	ast_ari_response_no_content(response);

}

void ast_ari_moh_stop_bridge(struct ast_variable *headers, struct ast_moh_stop_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge *, bridge, find_bridge(response, args->bridge_id), ao2_cleanup);

	if (!bridge) {
		/* the response is provided by find_bridge() */
		return;
	}

	if (stasis_app_bridge_moh_stop(bridge)) {
		ast_ari_response_error(
			response, 409, "Conflict",
			"Bridge isn't playing music");
		return;
	}

	ast_ari_response_no_content(response);
}

void ast_ari_get_bridge(struct ast_variable *headers, struct ast_get_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge_snapshot *, snapshot, ast_bridge_snapshot_get_latest(args->bridge_id), ao2_cleanup);
	if (!snapshot) {
		ast_ari_response_error(
			response, 404, "Not Found",
			"Bridge not found");
		return;
	}

	ast_ari_response_ok(response,
		ast_bridge_snapshot_to_json(snapshot));
}

void ast_ari_delete_bridge(struct ast_variable *headers, struct ast_delete_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge *, bridge, find_bridge(response, args->bridge_id), ao2_cleanup);
	if (!bridge) {
		return;
	}

	stasis_app_bridge_destroy(args->bridge_id);
	ast_ari_response_no_content(response);
}

void ast_ari_get_bridges(struct ast_variable *headers, struct ast_get_bridges_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct stasis_cache *, cache, NULL, ao2_cleanup);
	RAII_VAR(struct ao2_container *, snapshots, NULL, ao2_cleanup);
	RAII_VAR(struct ast_json *, json, NULL, ast_json_unref);
	struct ao2_iterator i;
	void *obj;

	cache = ast_bridge_cache();
	if (!cache) {
		ast_ari_response_error(
			response, 500, "Internal Server Error",
			"Message bus not initialized");
		return;
	}
	ao2_ref(cache, +1);

	snapshots = stasis_cache_dump(cache, ast_bridge_snapshot_type());
	if (!snapshots) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	json = ast_json_array_create();
	if (!json) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	i = ao2_iterator_init(snapshots, 0);
	while ((obj = ao2_iterator_next(&i))) {
		RAII_VAR(struct stasis_message *, msg, obj, ao2_cleanup);
		struct ast_bridge_snapshot *snapshot = stasis_message_data(msg);
		if (ast_json_array_append(json, ast_bridge_snapshot_to_json(snapshot))) {
			ast_ari_response_alloc_failed(response);
			return;
		}
	}
	ao2_iterator_destroy(&i);

	ast_ari_response_ok(response, ast_json_ref(json));
}

void ast_ari_new_bridge(struct ast_variable *headers, struct ast_new_bridge_args *args, struct ast_ari_response *response)
{
	RAII_VAR(struct ast_bridge *, bridge, stasis_app_bridge_create(args->type), ao2_cleanup);
	RAII_VAR(struct ast_bridge_snapshot *, snapshot, NULL, ao2_cleanup);

	if (!bridge) {
		ast_ari_response_error(
			response, 500, "Internal Error",
			"Unable to create bridge");
		return;
	}

	snapshot = ast_bridge_snapshot_create(bridge);
	if (!snapshot) {
		ast_ari_response_error(
			response, 500, "Internal Error",
			"Unable to create snapshot for new bridge");
		return;
	}

	ast_ari_response_ok(response,
		ast_bridge_snapshot_to_json(snapshot));
}
