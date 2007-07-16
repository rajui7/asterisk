/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
 * \brief App to transmit an image
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/translate.h"
#include "asterisk/image.h"
#include "asterisk/app.h"
#include "asterisk/options.h"

static char *app = "SendImage";

static char *synopsis = "Send an image file";

static char *descrip = 
"  SendImage(filename): Sends an image on a channel. \n"
"If the channel supports image transport but the image send\n"
"fails, the channel will be hung up. Otherwise, the dialplan\n"
"continues execution.\n"
"This application sets the following channel variable upon completion:\n"
"	SENDIMAGESTATUS		The status is the result of the attempt as a text string, one of\n"
"		OK | NOSUPPORT \n";			


static int sendimage_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	char *parse;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(filename);
		AST_APP_ARG(options);
	);
	
	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	if (ast_strlen_zero(args.filename)) {
		ast_log(LOG_WARNING, "SendImage requires an argument (filename[|options])\n");
		return -1;
	}

	if (args.options) {
	}

	if (!ast_supports_images(chan)) {
		/* Does not support transport */
		pbx_builtin_setvar_helper(chan, "SENDIMAGESTATUS", "NOSUPPORT");
		return 0;
	}

	if (!(res = ast_send_image(chan, args.filename)))
		pbx_builtin_setvar_helper(chan, "SENDIMAGESTATUS", "OK");
		
	return res;
}

static int unload_module(void)
{
	return ast_unregister_application(app);
}

static int load_module(void)
{
	return ast_register_application(app, sendimage_exec, synopsis, descrip);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Image Transmission Application");
