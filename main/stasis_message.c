/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2013, Digium, Inc.
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
 * \brief Stasis Message API.
 *
 * \author David M. Lee, II <dlee@digium.com>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/astobj2.h"
#include "asterisk/stasis.h"
#include "asterisk/utils.h"

/*! \internal */
struct stasis_message_type {
	struct stasis_message_vtable *vtable;
	char *name;
};

static struct stasis_message_vtable null_vtable = {};

static void message_type_dtor(void *obj)
{
	struct stasis_message_type *type = obj;
	ast_free(type->name);
	type->name = NULL;
}

struct stasis_message_type *stasis_message_type_create(const char *name,
	struct stasis_message_vtable *vtable)
{
	RAII_VAR(struct stasis_message_type *, type, NULL, ao2_cleanup);

	type = ao2_alloc(sizeof(*type), message_type_dtor);
	if (!type) {
		return NULL;
	}
	if (!vtable) {
		/* Null object pattern, FTW! */
		vtable = &null_vtable;
	}

	type->name = ast_strdup(name);
	if (!type->name) {
		return NULL;
	}
	type->vtable = vtable;

	ao2_ref(type, +1);
	return type;
}

const char *stasis_message_type_name(const struct stasis_message_type *type)
{
	return type->name;
}

/*! \internal */
struct stasis_message {
	/*! Time the message was created */
	struct timeval timestamp;
	/*! Type of the message */
	struct stasis_message_type *type;
	/*! Message content */
	void *data;
};

static void stasis_message_dtor(void *obj)
{
	struct stasis_message *message = obj;
	ao2_cleanup(message->type);
	ao2_cleanup(message->data);
}

struct stasis_message *stasis_message_create(struct stasis_message_type *type, void *data)
{
	RAII_VAR(struct stasis_message *, message, NULL, ao2_cleanup);

	if (type == NULL || data == NULL) {
		return NULL;
	}

	message = ao2_alloc(sizeof(*message), stasis_message_dtor);
	if (message == NULL) {
		return NULL;
	}

	message->timestamp = ast_tvnow();
	ao2_ref(type, +1);
	message->type = type;
	ao2_ref(data, +1);
	message->data = data;

	ao2_ref(message, +1);
	return message;
}

struct stasis_message_type *stasis_message_type(const struct stasis_message *msg)
{
	if (msg == NULL) {
		return NULL;
	}
	return msg->type;
}

void *stasis_message_data(const struct stasis_message *msg)
{
	if (msg == NULL) {
		return NULL;
	}
	return msg->data;
}

const struct timeval *stasis_message_timestamp(const struct stasis_message *msg)
{
	if (msg == NULL) {
		return NULL;
	}
	return &msg->timestamp;
}

#define INVOKE_VIRTUAL(fn, ...)				\
	({						\
		if (msg == NULL) {			\
			return NULL;			\
		}					\
		ast_assert(msg->type != NULL);		\
		ast_assert(msg->type->vtable != NULL);	\
		if (msg->type->vtable->fn == NULL) {	\
			return NULL;			\
		}					\
		msg->type->vtable->fn(__VA_ARGS__);	\
	})

struct ast_manager_event_blob *stasis_message_to_ami(struct stasis_message *msg)
{
	return INVOKE_VIRTUAL(to_ami, msg);
}

struct ast_json *stasis_message_to_json(struct stasis_message *msg)
{
	return INVOKE_VIRTUAL(to_json, msg);
}
